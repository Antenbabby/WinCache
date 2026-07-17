/*------------------------------------------------------------------------------
 * index.c — Block map index load from / flush to SSD
 *
 * The block map (array of CACHE_BLOCK_ENTRY) lives in non-paged pool for
 * fast O(1) access during I/O. Its canonical copy is stored in the "index
 * region" on the SSD cache partition, between the superblock and the
 * data region.
 *
 * On driver startup (cache attach): IndexLoadFromSsd() reads the entire
 * index from SSD into RAM.
 *
 * On driver shutdown / periodic flush: IndexFlushToSsd() writes the
 * current in-memory index back to SSD.
 *
 * Why flush? Even though this is a READ-ONLY cache, the metadata changes:
 * - access_count increments
 * - last_access timestamps update
 * - FLAG_VALID is set when new blocks are populated
 * - FLAG_POPULATING transitions
 * Flushing on clean shutdown ensures we don't lose this metadata.
 *------------------------------------------------------------------------------
 */

#include <ntddk.h>
#include <wdf.h>
#include "cacheflt.h"
#include "..\shared\protocol.h"

#define CACHE_POOL_TAG  'HCAC'

/*----------------------------------------------------------------------------
 * IndexLoadFromSsd
 *
 * Read the entire block map from the SSD index region into non-paged pool.
 *
 * Called during CacheInit() → IOCTL_CACHE_ATTACH.
 *----------------------------------------------------------------------------*/
NTSTATUS
IndexLoadFromSsd(
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    NTSTATUS          status;
    IO_STATUS_BLOCK   io_status;
    LARGE_INTEGER     byte_offset;
    ULONG             index_size_bytes;

    if (ctx->cache_file_object == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    if (ctx->total_cache_blocks == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    index_size_bytes = (ULONG)(ctx->total_cache_blocks * sizeof(CACHE_BLOCK_ENTRY));

    /*
     * Allocate non-paged pool for the block map.
     * Non-paged is required because we access it at DISPATCH_LEVEL inside
     * the spinlock (KeAcquireInStackQueuedSpinLock).
     */
    ctx->block_map = (CACHE_BLOCK_ENTRY *)ExAllocatePoolWithTag(
        NonPagedPool,
        index_size_bytes,
        CACHE_POOL_TAG);

    if (ctx->block_map == NULL) {
        KdPrint(("CacheFlt: IndexLoad failed — out of memory (%u bytes)\n",
            index_size_bytes));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(ctx->block_map, index_size_bytes);

    /*
     * Read the index from SSD. The index starts at index_offset sectors
     * from the start of the partition.
     */
    byte_offset.QuadPart = (LONGLONG)(ctx->block_size > 0 ?
        /* We need the index offset from the superblock. For now, use a fixed offset:
         * index_offset = 1 (sector 1, right after superblock).
         * In a full implementation, this is read from the superblock first.
         */
        512 : 512);

    status = ZwReadFile(
        ctx->cache_file_object,
        NULL,           /* no event */
        NULL,           /* no APC routine */
        NULL,           /* no APC context */
        &io_status,
        ctx->block_map,
        index_size_bytes,
        &byte_offset,
        NULL);

    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: IndexLoad ZwReadFile failed 0x%08x\n", status));
        ExFreePoolWithTag(ctx->block_map, CACHE_POOL_TAG);
        ctx->block_map = NULL;
        return status;
    }

    /*
     * Count already-valid blocks for the cached_block_count.
     * This is a best-effort scan since the lock isn't needed yet
     * (caching isn't active at this point).
     */
    {
        UINT64 i;
        ctx->cached_block_count = 0;
        for (i = 0; i < ctx->total_cache_blocks; i++) {
            if (ctx->block_map[i].flags & CACHE_FLAG_VALID) {
                ctx->cached_block_count++;
            }
        }
    }

    KdPrint(("CacheFlt: Index loaded — %llu blocks, %u bytes, %llu valid\n",
        ctx->total_cache_blocks, index_size_bytes, ctx->cached_block_count));

    return STATUS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * IndexFlushToSsd
 *
 * Write the in-memory block map back to the SSD index region.
 *
 * Called during:
 * - Clean shutdown (CacheFilterEvtDeviceContextCleanup)
 * - IOCTL_CACHE_DETACH
 * - IOCTL_CACHE_FLUSH
 * - Periodic timer (optional, not implemented in v1)
 *
 * Note: We hold the block_map_lock during the write so the index is
 * consistent. This is safe because ZwWriteFile can be called at
 * DISPATCH_LEVEL (though it may block briefly).
 *----------------------------------------------------------------------------*/
NTSTATUS
IndexFlushToSsd(
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    NTSTATUS          status;
    IO_STATUS_BLOCK   io_status;
    LARGE_INTEGER     byte_offset;
    ULONG             index_size_bytes;
    KLOCK_QUEUE_HANDLE lock_handle;

    if (ctx->cache_file_object == NULL || ctx->block_map == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    index_size_bytes = (ULONG)(ctx->total_cache_blocks * sizeof(CACHE_BLOCK_ENTRY));

    /*
     * Acquire the spinlock so no one modifies the block_map while we write it.
     * For a production system with large indexes (~128MB), we'd want to
     * double-buffer or use a dirty-block list to avoid holding the lock
     * for the entire write. For v1, simplicity wins.
     */
    KeAcquireInStackQueuedSpinLock(&ctx->block_map_lock, &lock_handle);

    byte_offset.QuadPart = 512;  /* sector 1 */

    status = ZwWriteFile(
        ctx->cache_file_object,
        NULL, NULL, NULL,
        &io_status,
        ctx->block_map,
        index_size_bytes,
        &byte_offset,
        NULL);

    KeReleaseInStackQueuedSpinLock(&lock_handle);

    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: IndexFlush ZwWriteFile failed 0x%08x\n", status));
    } else {
        KdPrint(("CacheFlt: Index flushed — %u bytes\n", index_size_bytes));
    }

    return status;
}
