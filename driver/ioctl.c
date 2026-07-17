/*------------------------------------------------------------------------------
 * ioctl.c — IOCTL dispatch handler
 *
 * Handles DeviceIoControl requests from the user-mode management tool.
 * These are sent via the filter device's control device object.
 *------------------------------------------------------------------------------
 */

#include <ntddk.h>
#include <wdf.h>
#include "cacheflt.h"
#include "..\shared\protocol.h"

/*----------------------------------------------------------------------------
 * CacheFilterEvtIoDeviceControl
 *
 * Main IOCTL dispatch. Routes to the appropriate handler based on the
 * IO control code.
 *----------------------------------------------------------------------------*/
VOID
CacheFilterEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    WDFDEVICE           device = WdfIoQueueGetDevice(Queue);
    CACHE_DISK_CONTEXT *ctx    = CacheDiskGetContext(device);
    NTSTATUS            status = STATUS_NOT_SUPPORTED;
    PVOID               input_buffer;
    PVOID               output_buffer;
    size_t              input_len;
    size_t              output_len;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    /*
     * Retrieve the input and output buffers. Since we use METHOD_BUFFERED,
     * both input and output share the same buffer (input comes first, output
     * overwrites it).
     */
    status = WdfRequestRetrieveInputBuffer(Request, 0, &input_buffer, &input_len);
    if (!NT_SUCCESS(status) && status != STATUS_BUFFER_TOO_SMALL) {
        /* No input buffer — that's fine for commands that don't need input */
        input_buffer = NULL;
        input_len = 0;
    }

    status = WdfRequestRetrieveOutputBuffer(Request, 0, &output_buffer, &output_len);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    /*
     * Route to the specific handler.
     */
    switch (IoControlCode) {

    case IOCTL_CACHE_ATTACH:
        status = CacheFilterIoctlAttach(
            ctx,
            (CACHE_ATTACH_INPUT *)input_buffer,
            (ULONG)input_len);
        break;

    case IOCTL_CACHE_DETACH:
        status = CacheFilterIoctlDetach(ctx);
        break;

    case IOCTL_CACHE_GET_STATS:
        status = CacheFilterIoctlGetStats(
            ctx,
            (CACHE_STATS_OUTPUT *)output_buffer,
            (ULONG)output_len);
        break;

    case IOCTL_CACHE_RESET_STATS:
        status = CacheFilterIoctlResetStats(ctx);
        break;

    case IOCTL_CACHE_GET_CONFIG:
        status = CacheFilterIoctlGetConfig(
            ctx,
            (CACHE_CONFIG_OUTPUT *)output_buffer,
            (ULONG)output_len);
        break;

    case IOCTL_CACHE_PRELOAD_RANGE:
        status = CacheFilterIoctlPreloadRange(
            ctx,
            (CACHE_PRELOAD_INPUT *)input_buffer,
            (ULONG)input_len);
        break;

    case IOCTL_CACHE_FLUSH:
        status = CacheFilterIoctlFlush(ctx);
        break;

    default:
        KdPrint(("CacheFlt: Unknown IOCTL 0x%08x\n", IoControlCode));
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, (status >= 0) ? output_len : 0);
}

/*----------------------------------------------------------------------------
 * CacheFilterIoctlAttach
 *
 * Activate caching on this disk. The user-mode manager sends the source
 * disk device path and the SSD cache partition device path.
 *
 * Phase 2+: This initializes the cache storage.
 * Phase 1: Stub — validates input, stores device paths.
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheFilterIoctlAttach(
    _In_ CACHE_DISK_CONTEXT *ctx,
    _In_ CACHE_ATTACH_INPUT *input,
    _In_ ULONG               input_len)
{
    if (input_len < sizeof(CACHE_ATTACH_INPUT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (input == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Prevent double-attach */
    if (ctx->caching_active) {
        KdPrint(("CacheFlt: Disk already has caching active\n"));
        return STATUS_ALREADY_REGISTERED;
    }

    KdPrint(("CacheFlt: Attach — source: %ws, cache: %ws, block_size: %u\n",
        input->header.source_disk,
        input->cache_partition,
        input->block_size));

    return CacheAttachToDisk(ctx, input);
}

/*----------------------------------------------------------------------------
 * CacheFilterIoctlDetach
 *
 * Deactivate caching on this disk. Flushes index to SSD and cleans up.
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheFilterIoctlDetach(
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    if (!ctx->caching_active) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    KdPrint(("CacheFlt: Detach requested\n"));

    /* Flush index to SSD before detaching */
    IndexFlushToSsd(ctx);

    /* Free block map */
    if (ctx->block_map != NULL) {
        ExFreePoolWithTag(ctx->block_map, 'HCAC');
        ctx->block_map = NULL;
    }

    /* Close cache partition */
    if (ctx->cache_file_object != NULL) {
        ObDereferenceObject(ctx->cache_file_object);
        ctx->cache_file_object = NULL;
    }

    ctx->caching_active = FALSE;
    ctx->total_cache_blocks = 0;
    ctx->cached_block_count = 0;

    return STATUS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * CacheFilterIoctlGetStats
 *
 * Return current cache statistics to user mode.
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheFilterIoctlGetStats(
    _In_  CACHE_DISK_CONTEXT *ctx,
    _Out_ CACHE_STATS_OUTPUT *output,
    _In_  ULONG               output_len)
{
    if (output_len < sizeof(CACHE_STATS_OUTPUT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    output->total_reads           = (UINT64)InterlockedAdd64(&ctx->total_reads, 0);
    output->cache_hits            = (UINT64)InterlockedAdd64(&ctx->cache_hits, 0);
    output->cache_misses          = (UINT64)InterlockedAdd64(&ctx->cache_misses, 0);
    output->bytes_read_total      = (UINT64)InterlockedAdd64(&ctx->bytes_read_total, 0);
    output->bytes_read_from_cache = (UINT64)InterlockedAdd64(&ctx->bytes_read_from_cache, 0);
    output->blocks_cached         = (UINT64)InterlockedAdd64((PLONGLONG)&ctx->cached_block_count, 0);
    output->blocks_total          = ctx->total_cache_blocks;
    output->populations_queued    = (UINT64)InterlockedAdd64(&ctx->populations_queued, 0);
    output->populations_completed = (UINT64)InterlockedAdd64(&ctx->populations_completed, 0);
    output->evictions             = (UINT64)InterlockedAdd64(&ctx->evictions, 0);

    /* Calculate hit rate */
    if (output->total_reads > 0) {
        output->hit_rate_percent_x100 = (output->cache_hits * 10000) / output->total_reads;
    } else {
        output->hit_rate_percent_x100 = 0;
    }

    return STATUS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * CacheFilterIoctlResetStats
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheFilterIoctlResetStats(
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    InterlockedExchange64(&ctx->total_reads, 0);
    InterlockedExchange64(&ctx->cache_hits, 0);
    InterlockedExchange64(&ctx->cache_misses, 0);
    InterlockedExchange64(&ctx->bytes_read_total, 0);
    InterlockedExchange64(&ctx->bytes_read_from_cache, 0);
    InterlockedExchange64(&ctx->populations_queued, 0);
    InterlockedExchange64(&ctx->populations_completed, 0);
    InterlockedExchange64(&ctx->evictions, 0);

    return STATUS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * CacheFilterIoctlGetConfig
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheFilterIoctlGetConfig(
    _In_  CACHE_DISK_CONTEXT  *ctx,
    _Out_ CACHE_CONFIG_OUTPUT *output,
    _In_  ULONG                output_len)
{
    if (output_len < sizeof(CACHE_CONFIG_OUTPUT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(output, sizeof(CACHE_CONFIG_OUTPUT));
    output->block_size          = ctx->block_size;
    output->is_active           = ctx->caching_active ? 1 : 0;
    output->total_cache_blocks  = ctx->total_cache_blocks;
    output->cached_blocks       = ctx->cached_block_count;

    return STATUS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * CacheFilterIoctlPreloadRange
 *
 * Pre-populate a range of LBAs into the cache. Used by the manager's
 * "preload" command to warm the cache for specific files.
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheFilterIoctlPreloadRange(
    _In_ CACHE_DISK_CONTEXT *ctx,
    _In_ CACHE_PRELOAD_INPUT *input,
    _In_ ULONG                input_len)
{
    if (input_len < sizeof(CACHE_PRELOAD_INPUT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (!ctx->caching_active) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    KdPrint(("CacheFlt: Preload range LBA %llu - %llu\n",
        input->start_lba, input->end_lba));

    /*
     * Phase 5: Queue populate work items for each block in range.
     */
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(input);
    return STATUS_NOT_IMPLEMENTED;
}

/*----------------------------------------------------------------------------
 * CacheFilterIoctlFlush
 *
 * Flush the in-memory block map index to SSD.
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheFilterIoctlFlush(
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    if (!ctx->caching_active) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    return IndexFlushToSsd(ctx);
}
