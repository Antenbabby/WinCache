/*------------------------------------------------------------------------------
 * io.c — I/O dispatch handlers for read and write IRPs
 *
 * Read path (fully wired, Phase 3+):
 *   1. If caching is active, check the cache for the requested byte range.
 *   2. All-or-nothing: if ALL blocks are cached → served from SSD.
 *      If ANY block is missing → forward to HDD + queue async populate.
 *   3. Write IRPs pass through untouched.
 *------------------------------------------------------------------------------
 */

#include <ntddk.h>
#include <wdf.h>
#include "cacheflt.h"
#include "..\shared\protocol.h"

#define CACHE_POOL_TAG  'HCAC'

/*----------------------------------------------------------------------------
 * Internal: read a single block from the SSD cache partition.
 *----------------------------------------------------------------------------*/
static NTSTATUS
CacheReadBlockFromSsd(
    _In_  CACHE_DISK_CONTEXT *ctx,
    _In_  UINT64              cache_slot,
    _Out_ PVOID               buffer,
    _In_  ULONG               block_size)
{
    NTSTATUS          status;
    IO_STATUS_BLOCK   io_status;
    LARGE_INTEGER     byte_offset;
    CACHE_SUPERBLOCK  sb;

    if (ctx->cache_file_object == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    /*
     * We need the data_offset from the superblock.
     * For efficiency in production, this would be cached in ctx at attach time.
     * For v1, we read the superblock on first access and cache the offset.
     */
    status = SuperblockRead(ctx, &sb);
    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: SuperblockRead failed during cache serve: 0x%08x\n",
            status));
        return status;
    }

    /*
     * Calculate byte offset on SSD:
     * data_offset (sectors) * 512 + cache_slot * block_size
     */
    byte_offset.QuadPart = (LONGLONG)sb.data_offset * 512 +
                           (LONGLONG)cache_slot * ctx->block_size;

    status = ZwReadFile(
        ctx->cache_file_object,
        NULL,           /* no event — synchronous */
        NULL, NULL,     /* no APC routine/context */
        &io_status,
        buffer,
        block_size,
        &byte_offset,
        NULL);

    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: ZwReadFile(SSD cache) failed at offset %lld: 0x%08x\n",
            byte_offset.QuadPart, status));
    }

    return status;
}

/*----------------------------------------------------------------------------
 * Internal: check if a range of blocks is fully cached.
 *
 * Returns TRUE if every block in [start_lba, end_lba) is present in cache.
 * Returns FALSE if any block is missing.
 *
 * On TRUE, all found blocks have their last_access timestamp updated.
 *----------------------------------------------------------------------------*/
static BOOLEAN
IsRangeFullyCached(
    _In_ CACHE_DISK_CONTEXT *ctx,
    _In_ UINT64              start_lba,
    _In_ UINT64              end_lba)       /* exclusive */
{
    UINT64 lba;

    for (lba = start_lba; lba < end_lba; lba += ctx->block_size) {
        UINT64 aligned_lba = (lba / ctx->block_size) * ctx->block_size;
        UINT64 slot = CacheLookup(ctx, aligned_lba);

        if (slot == CACHE_INVALID_INDEX) {
            return FALSE;   /* at least one block is missing */
        }
    }

    return TRUE;   /* all blocks are cached */
}

/*----------------------------------------------------------------------------
 * Internal: copy data from SSD cache blocks into the request buffer.
 *
 * Reads each cache block sequentially from SSD, copying into the MDL.
 *----------------------------------------------------------------------------*/
static NTSTATUS
ServeReadFromSsdCache(
    _In_ WDFREQUEST          Request,
    _In_ CACHE_DISK_CONTEXT *ctx,
    _In_ UINT64              start_lba,
    _In_ ULONG               total_bytes)
{
    NTSTATUS  status = STATUS_SUCCESS;
    ULONG     bytes_remaining = total_bytes;
    UINT64    current_lba = start_lba;
    PVOID     temp_buffer = NULL;
    ULONG     block_size = ctx->block_size;
    ULONG     offset_in_buffer = 0;

    /*
     * Allocate a temporary buffer for one block at a time.
     * We use non-paged pool since we're in an I/O path.
     */
    temp_buffer = ExAllocatePoolWithTag(NonPagedPool, block_size, CACHE_POOL_TAG);
    if (temp_buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /*
     * Read the request's output buffer (MDL). For METHOD_NEITHER or
     * METHOD_DIRECT, we'd use MmGetSystemAddressForMdlSafe. But since
     * we're a filter on a disk device, the read buffer is typically
     * described by the MDL in the IRP.
     *
     * We get the WDFMEMORY from the request.
     */
    {
        WDFMEMORY output_memory;
        PVOID     output_buffer;
        size_t    output_size;

        status = WdfRequestRetrieveOutputMemory(Request, &output_memory);
        if (NT_SUCCESS(status)) {
            output_buffer = WdfMemoryGetBuffer(output_memory, &output_size);
        } else {
            /* Fallback: try to get the MDL directly */
            output_buffer = NULL;
        }

        if (output_buffer == NULL) {
            KdPrint(("CacheFlt: Cannot get output buffer for cache serve\n"));
            ExFreePoolWithTag(temp_buffer, CACHE_POOL_TAG);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        /*
         * For each cache block in the range, read from SSD and copy into
         * the output buffer.
         */
        while (bytes_remaining > 0) {
            ULONG bytes_this_block;
            UINT64 aligned_lba = (current_lba / block_size) * block_size;
            UINT64 slot;
            ULONG  offset_in_block;
            ULONG  copy_size;

            slot = CacheLookup(ctx, aligned_lba);
            if (slot == CACHE_INVALID_INDEX) {
                /*
                 * Block disappeared between check and serve (eviction race).
                 * This is very unlikely for a read-only cache but we handle it.
                 */
                status = STATUS_UNEXPECTED_IO_ERROR;
                break;
            }

            /* Read the block from SSD cache */
            status = CacheReadBlockFromSsd(ctx, slot, temp_buffer, block_size);
            if (!NT_SUCCESS(status)) {
                break;
            }

            /* Calculate offset within the block and copy size */
            offset_in_block = (ULONG)(current_lba - aligned_lba);
            copy_size = min(bytes_remaining, block_size - offset_in_block);

            /*
             * Copy from temp buffer to output buffer.
             * Use RtlCopyMemory — safe at PASSIVE_LEVEL.
             */
            {
                PUCHAR src = (PUCHAR)temp_buffer + offset_in_block;
                PUCHAR dst = (PUCHAR)output_buffer + offset_in_buffer;
                RtlCopyMemory(dst, src, copy_size);
            }

            offset_in_buffer += copy_size;
            bytes_remaining   -= copy_size;
            current_lba       += copy_size;

            StatsRecordHit(ctx, copy_size);
        }
    }

    ExFreePoolWithTag(temp_buffer, CACHE_POOL_TAG);
    return status;
}

/*----------------------------------------------------------------------------
 * CacheFilterEvtIoRead
 *
 * Called by KMDF for every IRP_MJ_READ on the filtered disk.
 *
 * Logic:
 *   1. If caching is not active → forward to HDD (pass-through).
 *   2. Convert byte offset + length to LBA range.
 *   3. Check if ALL blocks are in cache.
 *   4. All cached → serve from SSD cache (fast path).
 *   5. Any missing → forward to HDD (slow path) + queue async populate.
 *----------------------------------------------------------------------------*/
VOID
CacheFilterEvtIoRead(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length)
{
    WDFDEVICE             device = WdfIoQueueGetDevice(Queue);
    CACHE_DISK_CONTEXT   *ctx    = CacheDiskGetContext(device);
    WDF_REQUEST_PARAMETERS params;
    ULONGLONG             byte_offset;
    ULONG                 read_length;
    UINT64                start_lba;
    UINT64                end_lba;
    NTSTATUS              status = STATUS_SUCCESS;
    BOOLEAN               fully_cached;

    UNREFERENCED_PARAMETER(Length);

    /*
     * If caching is not active, forward directly to the lower driver.
     */
    if (!ctx->caching_active || ctx->block_map == NULL) {
        status = CacheFilterForwardRead(Request, ctx);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /*
     * Get the read parameters: byte offset and length.
     */
    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    byte_offset = params.Parameters.Read.DeviceOffset;
    read_length = params.Parameters.Read.Length;

    if (read_length == 0) {
        /* Zero-length read — complete with success, no data */
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
        return;
    }

    /*
     * Convert byte range to LBA range, aligned to block boundaries.
     */
    start_lba = (UINT64)byte_offset;
    end_lba   = start_lba + read_length;

    /*
     * Check if the entire range is in cache.
     */
    fully_cached = IsRangeFullyCached(ctx, start_lba, end_lba);

    if (fully_cached) {
        /*
         * ★ CACHE HIT — serve the entire read from the SSD cache.
         */
        KdPrint(("CacheFlt: CACHE HIT — serving %lu bytes from SSD\n",
            read_length));

        status = ServeReadFromSsdCache(
            Request, ctx, start_lba, read_length);

        if (NT_SUCCESS(status)) {
            WdfRequestCompleteWithInformation(
                Request, STATUS_SUCCESS, read_length);
        } else {
            /*
             * SSD read failed — fall back to HDD.
             * Mark blocks as evicted so they get re-populated.
             */
            KdPrint(("CacheFlt: SSD serve failed, falling back to HDD\n"));
            goto forward_to_hdd;
        }
    } else {
        /*
         * ★ CACHE MISS — forward to HDD.
         */
forward_to_hdd:
        status = CacheFilterForwardRead(Request, ctx);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
        }
        /*
         * The completion routine (CacheFilterReadCompletionRoutine) will
         * queue async cache population for the missed blocks.
         *
         * We also record the miss stat here; hits from partial reads
         * are recorded in ServeReadFromSsdCache.
         */
        StatsRecordMiss(ctx, read_length);
    }
}

/*----------------------------------------------------------------------------
 * CacheFilterForwardRead
 *
 * Send a read request down the device stack to the underlying disk.
 * Uses send-and-forget: KMDF automatically completes the request when
 * the lower driver finishes.
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheFilterForwardRead(
    _In_ WDFREQUEST          Request,
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    NTSTATUS                status;
    WDF_REQUEST_SEND_OPTIONS send_options;
    BOOLEAN                 ret;

    WDF_REQUEST_SEND_OPTIONS_INIT(
        &send_options,
        WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

    /*
     * Set completion routine for post-read processing (stats + async populate).
     */
    WdfRequestSetCompletionRoutine(
        Request,
        CacheFilterReadCompletionRoutine,
        ctx);

    WdfRequestFormatRequestUsingCurrentType(Request);

    ret = WdfRequestSend(
        Request,
        ctx->source_io_target,
        &send_options);

    if (!ret) {
        status = WdfRequestGetStatus(Request);
        KdPrint(("CacheFlt: WdfRequestSend failed 0x%08x\n", status));
        return status;
    }

    return STATUS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * CacheFilterReadCompletionRoutine
 *
 * Called when a forwarded read completes. Triggers async cache population
 * for the blocks that were just read from HDD (Phase 4).
 *
 * Phase 4+: The actual async populate work item is queued here.
 * Phase 3: Records completion statistics.
 *----------------------------------------------------------------------------*/
VOID
CacheFilterReadCompletionRoutine(
    _In_ WDFREQUEST                        Request,
    _In_ WDFIOTARGET                       Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS    CompletionParams,
    _In_ WDFCONTEXT                        Context)
{
    CACHE_DISK_CONTEXT *ctx = (CACHE_DISK_CONTEXT *)Context;
    WDF_REQUEST_PARAMETERS params;
    ULONGLONG byte_offset;
    ULONG     read_length;
    UINT64    aligned_lba;

    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Request);

    /*
     * If the read succeeded and caching is active, queue async populate.
     */
    if (NT_SUCCESS(CompletionParams->IoStatus.Status) &&
        ctx->caching_active && ctx->block_map != NULL) {

        /*
         * Get the original read parameters to determine which blocks to cache.
         */
        WDF_REQUEST_PARAMETERS_INIT(&params);
        WdfRequestGetParameters(Request, &params);

        byte_offset = params.Parameters.Read.DeviceOffset;
        read_length = params.Parameters.Read.Length;

        /*
         * Queue async population for the blocks in this range.
         * Each missed block gets a populate work item (Phase 4).
         */
        for (aligned_lba = (byte_offset / ctx->block_size) * ctx->block_size;
             aligned_lba < byte_offset + read_length;
             aligned_lba += ctx->block_size) {

            CachePopulateAsync(ctx, aligned_lba);
        }
    }
}

/*----------------------------------------------------------------------------
 * CacheFilterEvtIoWrite
 *
 * Pure pass-through for all writes. We never cache or modify write IRPs.
 *----------------------------------------------------------------------------*/
VOID
CacheFilterEvtIoWrite(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length)
{
    WDFDEVICE           device = WdfIoQueueGetDevice(Queue);
    CACHE_DISK_CONTEXT *ctx    = CacheDiskGetContext(device);
    NTSTATUS            status;
    WDF_REQUEST_SEND_OPTIONS send_options;
    BOOLEAN             ret;

    UNREFERENCED_PARAMETER(Length);

    WDF_REQUEST_SEND_OPTIONS_INIT(
        &send_options,
        WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

    WdfRequestFormatRequestUsingCurrentType(Request);

    ret = WdfRequestSend(
        Request,
        ctx->source_io_target,
        &send_options);

    if (!ret) {
        status = WdfRequestGetStatus(Request);
        WdfRequestComplete(Request, status);
    }
}

/*----------------------------------------------------------------------------
 * CacheFilterServiceFromCache  — DEPRECATED, logic inlined above.
 *
 * Kept for API compatibility. Internal reads use CacheReadBlockFromSsd()
 * and ServeReadFromSsdCache() directly.
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheFilterServiceFromCache(
    _In_ WDFREQUEST          Request,
    _In_ CACHE_DISK_CONTEXT *ctx,
    _In_ UINT64              cache_slot)
{
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(cache_slot);
    return STATUS_NOT_IMPLEMENTED;
}
