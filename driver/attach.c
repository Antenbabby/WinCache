/*------------------------------------------------------------------------------
 * attach.c — Attach/detach logic for the cache filter
 *
 * "Attach" means: open the SSD cache partition, validate/init the superblock,
 * load the block map, allocate work items, and start caching.
 *
 * "Detach" means: stop caching, flush index to SSD, free resources.
 *
 * This is the bridge between the IOCTL handlers (ioctl.c) and the cache
 * subsystem (cache.c, superblock.c, index.c).
 *------------------------------------------------------------------------------
 */

#include <ntddk.h>
#include <wdf.h>
#include "cacheflt.h"
#include "..\shared\protocol.h"

#define CACHE_POOL_TAG  'HCAC'

/*----------------------------------------------------------------------------
 * CacheAttachToDisk
 *
 * Activate the cache for this disk. Steps:
 * 1. Parse input parameters (block size, source disk, cache partition)
 * 2. Open the cache SSD partition via ZwCreateFile
 * 3. Read and validate the superblock
 * 4. Load the block map index from SSD → non-paged pool
 * 5. Initialize eviction cursor
 * 6. Mark caching_active = TRUE
 *
 * Phase 2+: Full implementation.
 * Phase 1: Stub — stores device paths.
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheAttachToDisk(
    _In_ CACHE_DISK_CONTEXT *ctx,
    _In_ CACHE_ATTACH_INPUT *input)
{
    NTSTATUS              status;
    UNICODE_STRING        cache_partition_name;
    OBJECT_ATTRIBUTES     obj_attrs;
    IO_STATUS_BLOCK       io_status;
    HANDLE                cache_handle;
    CACHE_SUPERBLOCK      superblock;
    UINT32                block_size;
    UINT64                total_blocks;

    KdPrint(("CacheFlt: CacheAttachToDisk\n"));

    /*
     * Determine block size — use input value, or default.
     */
    block_size = input->block_size;
    if (block_size == 0) {
        block_size = CACHE_DEFAULT_BLOCK_SIZE;
    }

    /* Must be power of 2 and at least 4096 */
    if ((block_size & (block_size - 1)) != 0 || block_size < 4096) {
        KdPrint(("CacheFlt: Invalid block size %u\n", block_size));
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * Open the cache SSD partition.
     *
     * We need a kernel handle for raw block I/O. ZwCreateFile is used
     * because we're in an arbitrary thread context (IOCTL handler).
     *
     * The partition is specified as a device path like:
     *   \Device\Harddisk3\Partition1
     *
     * We open with DASD (direct access storage device) semantics:
     * FILE_READ_DATA | FILE_WRITE_DATA with SYNCHRONIZE for sync I/O.
     */
    RtlInitUnicodeString(&cache_partition_name, input->cache_partition);

    InitializeObjectAttributes(
        &obj_attrs,
        &cache_partition_name,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,   /* root directory */
        NULL);  /* security descriptor */

    status = ZwCreateFile(
        &cache_handle,
        FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
        &obj_attrs,
        &io_status,
        NULL,                   /* allocation size — not needed */
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN,              /* must already exist */
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
        NULL,                   /* extended attributes */
        0);                     /* extended attributes length */

    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: ZwCreateFile(\"%wZ\") failed 0x%08x\n",
            &cache_partition_name, status));
        return status;
    }

    /*
     * Get the file object from the handle so we can use ZwReadFile/ZwWriteFile.
     */
    {
        PFILE_OBJECT file_object;
        status = ObReferenceObjectByHandle(
            cache_handle,
            FILE_READ_DATA | FILE_WRITE_DATA,
            *IoFileObjectType,
            KernelMode,
            (PVOID *)&file_object,
            NULL);

        if (!NT_SUCCESS(status)) {
            KdPrint(("CacheFlt: ObReferenceObjectByHandle failed 0x%08x\n", status));
            ZwClose(cache_handle);
            return status;
        }

        ctx->cache_file_object = file_object;
        ctx->cache_device_object = IoGetRelatedDeviceObject(file_object);
    }

    /*
     * Close the Zw handle — we use the file object reference from now on.
     */
    ZwClose(cache_handle);

    /*
     * Read and validate the superblock from the cache partition.
     */
    status = SuperblockRead(ctx, &superblock);
    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: SuperblockRead failed 0x%08x — cache may not be initialized\n", status));
        goto cleanup_file;
    }

    /*
     * Configure from superblock values.
     */
    ctx->block_size         = superblock.block_size;
    ctx->total_cache_blocks = superblock.total_cache_blocks;

    /*
     * Load the block map from SSD into non-paged pool.
     */
    status = IndexLoadFromSsd(ctx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: IndexLoadFromSsd failed 0x%08x\n", status));
        goto cleanup_file;
    }

    /*
     * Allocate work items for async cache population and index sync.
     */
    ctx->populate_work_item = IoAllocateWorkItem(ctx->source_physical_device);
    if (ctx->populate_work_item == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto cleanup_blockmap;
    }

    ctx->index_sync_work_item = IoAllocateWorkItem(ctx->source_physical_device);
    if (ctx->index_sync_work_item == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto cleanup_populate_wi;
    }

    /*
     * Initialize the CLOCK LRU cursor.
     */
    LruClockResetCursor(ctx);

    /*
     * Activate caching.
     */
    ctx->caching_active = TRUE;

    KdPrint(("CacheFlt: Cache activated — block_size=%u, total_blocks=%llu\n",
        ctx->block_size, ctx->total_cache_blocks));

    return STATUS_SUCCESS;

cleanup_populate_wi:
    IoFreeWorkItem(ctx->populate_work_item);
    ctx->populate_work_item = NULL;

cleanup_blockmap:
    ExFreePoolWithTag(ctx->block_map, CACHE_POOL_TAG);
    ctx->block_map = NULL;

cleanup_file:
    ObDereferenceObject(ctx->cache_file_object);
    ctx->cache_file_object = NULL;
    return status;
}

/*----------------------------------------------------------------------------
 * CacheDetachFromDisk
 *
 * Deactivate caching and release resources. Called from
 * CacheFilterIoctlDetach() which handles the flush and free.
 *
 * This function performs the reverse of CacheAttachToDisk:
 * 1. Sets caching_active = FALSE (stops all new cache operations)
 * 2. Flushes index to SSD
 * 3. Frees block map
 * 4. Closes cache file object
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheDetachFromDisk(
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    KdPrint(("CacheFlt: CacheDetachFromDisk\n"));

    ctx->caching_active = FALSE;

    /*
     * Flush the index to SSD one last time.
     */
    if (ctx->block_map != NULL) {
        IndexFlushToSsd(ctx);
    }

    /*
     * Free the block map.
     */
    if (ctx->block_map != NULL) {
        ExFreePoolWithTag(ctx->block_map, CACHE_POOL_TAG);
        ctx->block_map = NULL;
    }

    /*
     * Free work items.
     */
    if (ctx->populate_work_item != NULL) {
        IoFreeWorkItem(ctx->populate_work_item);
        ctx->populate_work_item = NULL;
    }
    if (ctx->index_sync_work_item != NULL) {
        IoFreeWorkItem(ctx->index_sync_work_item);
        ctx->index_sync_work_item = NULL;
    }

    /*
     * Close the cache partition file object.
     */
    if (ctx->cache_file_object != NULL) {
        ObDereferenceObject(ctx->cache_file_object);
        ctx->cache_file_object = NULL;
    }

    ctx->block_size = 0;
    ctx->total_cache_blocks = 0;
    ctx->cached_block_count = 0;

    KdPrint(("CacheFlt: Cache detached\n"));
    return STATUS_SUCCESS;
}
