/*------------------------------------------------------------------------------
 * cache.c — Core cache logic: init, lookup, populate, evict
 *
 * CacheLookup:      Direct-mapped hash with linear probing (Phase 3)
 * CachePopulateAsync: Work-item-based async fill from HDD → SSD (Phase 4)
 * CacheSelectEvictionSlot: CLOCK LRU wrapper
 *------------------------------------------------------------------------------
 */

#include <ntddk.h>
#include <wdf.h>
#include "cacheflt.h"
#include "..\shared\protocol.h"

#define CACHE_POOL_TAG  'HCAC'

/*----------------------------------------------------------------------------
 * Internal: work item routine for async cache population.
 *
 * Runs at PASSIVE_LEVEL in a system worker thread. The context is freed
 * after completion (single-use work item pattern).
 *
 * Steps:
 *   1. Find a slot to evict (or empty slot)
 *   2. Read the block from the source HDD
 *   3. Write the block to the SSD cache partition
 *   4. Update the block_map entry
 *----------------------------------------------------------------------------*/
static VOID
CachePopulateWorkItemRoutine(
    _In_ PDEVICE_OBJECT  DeviceObject,
    _In_opt_ PVOID       Context)
{
    POPULATE_WORK_ITEM_CONTEXT *work_ctx;
    CACHE_DISK_CONTEXT        *ctx;
    UINT64                     source_lba;
    UINT64                     cache_slot;
    NTSTATUS                   status;
    IO_STATUS_BLOCK            io_status;
    LARGE_INTEGER              byte_offset;
    PVOID                      block_buffer = NULL;
    CACHE_SUPERBLOCK           sb;
    KLOCK_QUEUE_HANDLE         lock_handle;
    UINT64                     evicted_lba;

    UNREFERENCED_PARAMETER(DeviceObject);

    work_ctx  = (POPULATE_WORK_ITEM_CONTEXT *)Context;
    ctx       = work_ctx->disk_ctx;
    source_lba = work_ctx->source_lba;

    if (ctx == NULL || !ctx->caching_active) {
        goto cleanup;
    }

    /*
     * Allocate a temporary buffer for the block.
     */
    block_buffer = ExAllocatePoolWithTag(
        NonPagedPool, ctx->block_size, CACHE_POOL_TAG);
    if (block_buffer == NULL) {
        KdPrint(("CacheFlt: PopulateWorkItem — OOM for %u bytes\n",
            ctx->block_size));
        goto cleanup;
    }

    /*
     * --- Step 1: Find a cache slot ---
     *
     * First, check if there's already a slot for this LBA (race: another
     * work item may have populated it while we were queued).
     * If not, find an empty slot or evict one.
     */
    {
        cache_slot = CacheLookup(ctx, source_lba);
        if (cache_slot != CACHE_INVALID_INDEX) {
            /* Already cached — nothing to do */
            KdPrint(("CacheFlt: Populate — block already cached, skipping\n"));
            ExFreePoolWithTag(block_buffer, CACHE_POOL_TAG);
            ExFreePoolWithTag(work_ctx, CACHE_POOL_TAG);
            return;
        }

        cache_slot = CacheSelectEvictionSlot(ctx);
        if (cache_slot == CACHE_INVALID_INDEX) {
            KdPrint(("CacheFlt: Populate — no evictable slot (cache full + all pinned)\n"));
            goto cleanup;
        }

        /*
         * Mark the slot as POPULATING so lookups skip it.
         */
        KeAcquireInStackQueuedSpinLock(&ctx->block_map_lock, &lock_handle);
        ctx->block_map[cache_slot].flags &= ~CACHE_FLAG_VALID;
        ctx->block_map[cache_slot].flags |= CACHE_FLAG_POPULATING;
        evicted_lba = ctx->block_map[cache_slot].source_lba;
        KeReleaseInStackQueuedSpinLock(&lock_handle);
    }

    /*
     * --- Step 2: Read the block from the source HDD ---
     *
     * We use ZwReadFile on the source disk's file object.
     * The source_physical_device is the DEVICE_OBJECT we filter.
     *
     * To read from it, we need a file object. For simplicity in v1,
     * we use IoGetDeviceObjectPointer to get a reference.
     *
     * Note: This requires the source disk to be accessible at PASSIVE_LEVEL.
     */
    {
        PFILE_OBJECT source_file = NULL;
        PDEVICE_OBJECT source_dev = NULL;

        /*
         * Get a file object for the source disk.
         * The source physical device was stored at attach time.
         */
        if (ctx->source_physical_device == NULL) {
            KdPrint(("CacheFlt: Populate — no source device\n"));
            goto abort_populate;
        }

        /*
         * For v1, we use the fact that as a lower filter, we can send
         * IRPs directly to the next lower driver. But for a work item,
         * it's simpler to use IoGetDeviceObjectPointer.
         *
         * Actually, the simpler path: we already have ctx->source_io_target
         * as a WDFIOTARGET. But WDF I/O targets require WDFREQUEST objects.
         *
         * For the populate path, we use ZwReadFile on the raw disk device.
         * We need a reference to the device object.
         */
        status = IoGetDeviceObjectPointer(
            NULL,                           /* no name — use device directly */
            FILE_READ_DATA,
            &source_file,
            &source_dev);

        /*
         * IoGetDeviceObjectPointer requires a device name, which we don't
         * have stored. Alternative: use the source_physical_device directly
         * by building an IRP and sending it.
         *
         * For v1 simplicity, we build a synchronous IRP:
         */
        if (!NT_SUCCESS(status)) {
            /* Fallback: build and send a direct IRP */
            PIRP  irp;
            KEVENT event;
            LARGE_INTEGER src_offset;

            KeInitializeEvent(&event, NotificationEvent, FALSE);

            src_offset.QuadPart = (LONGLONG)source_lba;

            irp = IoBuildSynchronousFsdRequest(
                IRP_MJ_READ,
                ctx->source_physical_device,
                block_buffer,
                ctx->block_size,
                &src_offset,
                &event,
                &io_status);

            if (irp == NULL) {
                KdPrint(("CacheFlt: Populate — IoBuildSynchronousFsdRequest failed\n"));
                goto abort_populate;
            }

            status = IoCallDriver(ctx->source_physical_device, irp);

            if (status == STATUS_PENDING) {
                KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
                status = io_status.Status;
            }
        } else {
            /* Use ZwReadFile on the opened source file */
            LARGE_INTEGER src_offset;
            src_offset.QuadPart = (LONGLONG)source_lba;

            status = ZwReadFile(
                source_file,
                NULL, NULL, NULL,
                &io_status,
                block_buffer,
                ctx->block_size,
                &src_offset,
                NULL);

            ObDereferenceObject(source_file);
        }

        if (!NT_SUCCESS(status)) {
            KdPrint(("CacheFlt: Populate — source read failed LBA %llu: 0x%08x\n",
                source_lba, status));
            goto abort_populate;
        }
    }

    /*
     * --- Step 3: Write the block to the SSD cache partition ---
     */
    {
        status = SuperblockRead(ctx, &sb);
        if (!NT_SUCCESS(status)) {
            KdPrint(("CacheFlt: Populate — superblock read failed\n"));
            goto abort_populate;
        }

        byte_offset.QuadPart = (LONGLONG)sb.data_offset * 512 +
                               (LONGLONG)cache_slot * ctx->block_size;

        status = ZwWriteFile(
            ctx->cache_file_object,
            NULL, NULL, NULL,
            &io_status,
            block_buffer,
            ctx->block_size,
            &byte_offset,
            NULL);

        if (!NT_SUCCESS(status)) {
            KdPrint(("CacheFlt: Populate — SSD write failed slot %llu: 0x%08x\n",
                cache_slot, status));
            goto abort_populate;
        }
    }

    /*
     * --- Step 4: Update the block_map ---
     */
    {
        KeAcquireInStackQueuedSpinLock(&ctx->block_map_lock, &lock_handle);
        ctx->block_map[cache_slot].source_lba  = source_lba;
        ctx->block_map[cache_slot].last_access = KeQueryTickCount();
        ctx->block_map[cache_slot].access_count = 0;
        ctx->block_map[cache_slot].flags       = CACHE_FLAG_VALID | CACHE_FLAG_ACCESSED;
        ctx->cached_block_count++;
        KeReleaseInStackQueuedSpinLock(&lock_handle);
    }

    StatsRecordPopulationComplete(ctx);
    KdPrint(("CacheFlt: Populate — LBA %llu → slot %llu (block %llu KB)\n",
        source_lba, cache_slot, ctx->block_size / 1024));

    ExFreePoolWithTag(block_buffer, CACHE_POOL_TAG);
    ExFreePoolWithTag(work_ctx, CACHE_POOL_TAG);
    return;

abort_populate:
    /*
     * Restore the slot to empty (not valid, not populating).
     */
    KeAcquireInStackQueuedSpinLock(&ctx->block_map_lock, &lock_handle);
    ctx->block_map[cache_slot].flags &= ~(CACHE_FLAG_VALID | CACHE_FLAG_POPULATING);
    KeReleaseInStackQueuedSpinLock(&lock_handle);

cleanup:
    if (block_buffer) {
        ExFreePoolWithTag(block_buffer, CACHE_POOL_TAG);
    }
    if (work_ctx) {
        ExFreePoolWithTag(work_ctx, CACHE_POOL_TAG);
    }
}

/*----------------------------------------------------------------------------
 * CacheInit
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheInit(
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    KdPrint(("CacheFlt: CacheInit\n"));
    return STATUS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * CacheTeardown
 *----------------------------------------------------------------------------*/
VOID
CacheTeardown(
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    KdPrint(("CacheFlt: CacheTeardown\n"));
}

/*----------------------------------------------------------------------------
 * CacheLookup
 *
 * Look up a source LBA in the block map. Direct-mapped hash with
 * linear probing (up to CACHE_LINEAR_PROBE_MAX probes).
 *
 * Must be called at IRQL where spinlocks are safe (PASSIVE_LEVEL or
 * DISPATCH_LEVEL — uses InStackQueuedSpinLock).
 *
 * Returns: block map index on HIT, or CACHE_INVALID_INDEX on MISS.
 * Side effect: On HIT, updates last_access and access_count.
 *----------------------------------------------------------------------------*/
UINT64
CacheLookup(
    _In_ CACHE_DISK_CONTEXT *ctx,
    _In_ UINT64              source_lba)
{
    UINT64  block_index;
    UINT64  probe;
    UINT64  slot;
    KLOCK_QUEUE_HANDLE lock_handle;

    if (ctx->block_map == NULL || ctx->total_cache_blocks == 0) {
        return CACHE_INVALID_INDEX;
    }

    /* Align LBA to block boundary */
    block_index = source_lba / ctx->block_size;

    /* Direct-mapped hash */
    slot = block_index % ctx->total_cache_blocks;

    KeAcquireInStackQueuedSpinLock(&ctx->block_map_lock, &lock_handle);

    for (probe = 0; probe < CACHE_LINEAR_PROBE_MAX; probe++) {
        UINT64 current_slot = (slot + probe) % ctx->total_cache_blocks;
        UINT32 flags = ctx->block_map[current_slot].flags;

        /*
         * Check for match: valid, not populating, correct LBA
         */
        if ((flags & CACHE_FLAG_VALID) &&
            !(flags & CACHE_FLAG_POPULATING) &&
            ctx->block_map[current_slot].source_lba == source_lba) {

            /* HIT! */
            ctx->block_map[current_slot].last_access = KeQueryTickCount();
            ctx->block_map[current_slot].flags |= CACHE_FLAG_ACCESSED;
            ctx->block_map[current_slot].access_count++;

            KeReleaseInStackQueuedSpinLock(&lock_handle);
            return current_slot;
        }

        /*
         * Empty slot (no VALID flag, not being populated) —
         * chain ends here for direct-mapped lookup.
         */
        if (!(flags & CACHE_FLAG_VALID) &&
            !(flags & CACHE_FLAG_POPULATING)) {
            break;
        }
    }

    KeReleaseInStackQueuedSpinLock(&lock_handle);
    return CACHE_INVALID_INDEX;
}

/*----------------------------------------------------------------------------
 * CacheMarkAccessed
 *----------------------------------------------------------------------------*/
VOID
CacheMarkAccessed(
    _In_ CACHE_DISK_CONTEXT *ctx,
    _In_ UINT64              slot)
{
    KLOCK_QUEUE_HANDLE lock_handle;

    if (slot >= ctx->total_cache_blocks) {
        return;
    }

    KeAcquireInStackQueuedSpinLock(&ctx->block_map_lock, &lock_handle);
    ctx->block_map[slot].last_access = KeQueryTickCount();
    ctx->block_map[slot].flags |= CACHE_FLAG_ACCESSED;
    KeReleaseInStackQueuedSpinLock(&lock_handle);
}

/*----------------------------------------------------------------------------
 * CachePopulateAsync
 *
 * Queue an async work item to populate a missed block from HDD → SSD.
 *
 * Called from the read completion routine at DISPATCH_LEVEL. We allocate
 * a POPULATE_WORK_ITEM_CONTEXT and queue the work item to run at PASSIVE_LEVEL.
 *
 * To avoid overwhelming the system with work items for large sequential
 * reads, we track populations_queued and skip if too many are in flight.
 *----------------------------------------------------------------------------*/
NTSTATUS
CachePopulateAsync(
    _In_ CACHE_DISK_CONTEXT *ctx,
    _In_ UINT64              source_lba)
{
    POPULATE_WORK_ITEM_CONTEXT *work_ctx;

    /*
     * Skip if caching is not active.
     */
    if (!ctx->caching_active || ctx->populate_work_item == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    /*
     * Rate-limit: don't queue more than 1024 outstanding populations.
     */
    if (InterlockedAdd64(&ctx->populations_queued, 0) -
        InterlockedAdd64(&ctx->populations_completed, 0) > 1024) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /*
     * Allocate a work item context.
     */
    work_ctx = (POPULATE_WORK_ITEM_CONTEXT *)ExAllocatePoolWithTag(
        NonPagedPool, sizeof(POPULATE_WORK_ITEM_CONTEXT), CACHE_POOL_TAG);
    if (work_ctx == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    work_ctx->source_lba = source_lba;
    work_ctx->cache_slot = CACHE_INVALID_INDEX;  /* will be selected by work item */
    work_ctx->disk_ctx   = ctx;

    InterlockedIncrement64(&ctx->populations_queued);

    /*
     * Queue the work item. It runs at PASSIVE_LEVEL in a system worker thread.
     */
    IoQueueWorkItem(
        ctx->populate_work_item,
        CachePopulateWorkItemRoutine,
        WORK_QUEUE_ITEM_FLAG_DELAYED,    /* runs at PASSIVE_LEVEL */
        work_ctx);

    return STATUS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * CacheSelectEvictionSlot
 *
 * Find a cache slot to evict. Uses CLOCK LRU from lru.c.
 *
 * Must be called with block_map_lock NOT held (LruClockFindVictim acquires it).
 * Returns: slot index, or CACHE_INVALID_INDEX if nothing available.
 *----------------------------------------------------------------------------*/
UINT64
CacheSelectEvictionSlot(
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    KLOCK_QUEUE_HANDLE lock_handle;
    UINT64             slot;

    KeAcquireInStackQueuedSpinLock(&ctx->block_map_lock, &lock_handle);
    slot = LruClockFindVictim(ctx);
    KeReleaseInStackQueuedSpinLock(&lock_handle);

    return slot;
}
