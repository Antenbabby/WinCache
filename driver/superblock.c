/*------------------------------------------------------------------------------
 * superblock.c — Superblock read, write, validate, initialize
 *
 * The superblock occupies sector 0 of the cache SSD partition. It contains
 * the magic number, version, block size, cache capacity, and index/data offsets.
 *
 * We read/write it using ZwReadFile/ZwWriteFile against the raw partition.
 *------------------------------------------------------------------------------
 */

#include <ntddk.h>
#include <wdf.h>
#include "cacheflt.h"
#include "..\shared\protocol.h"

/*
 * Memory pool tag.
 */
#define CACHE_POOL_TAG  'HCAC'

/*----------------------------------------------------------------------------
 * SuperblockRead
 *
 * Read the superblock from sector 0 of the cache SSD partition.
 *
 * We use ZwReadFile against the cached partition file object. Since this is
 * called during IOCTL_CACHE_ATTACH (user-mode context), we can use synchronous
 * I/O with KeWaitForSingleObject.
 *----------------------------------------------------------------------------*/
NTSTATUS
SuperblockRead(
    _In_  CACHE_DISK_CONTEXT *ctx,
    _Out_ CACHE_SUPERBLOCK   *sb)
{
    NTSTATUS          status;
    IO_STATUS_BLOCK   io_status;
    LARGE_INTEGER     byte_offset;

    if (ctx->cache_file_object == NULL) {
        KdPrint(("CacheFlt: SuperblockRead — no cache file object\n"));
        return STATUS_INVALID_HANDLE;
    }

    RtlZeroMemory(sb, sizeof(CACHE_SUPERBLOCK));

    /*
     * Sector 0 = byte offset 0.
     */
    byte_offset.QuadPart = 0;

    status = ZwReadFile(
        ctx->cache_file_object,     /* handle from ZwCreateFile */
        NULL,                        /* no event (sync) */
        NULL,                        /* no APC routine */
        NULL,                        /* no APC context */
        &io_status,
        sb,
        sizeof(CACHE_SUPERBLOCK),
        &byte_offset,
        NULL);                       /* no key */

    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: SuperblockRead ZwReadFile failed 0x%08x\n", status));
        return status;
    }

    /*
     * Validate the magic number.
     */
    if (sb->magic != CACHE_MAGIC) {
        KdPrint(("CacheFlt: SuperblockRead — bad magic: 0x%016llx, expected 0x%016llx\n",
            sb->magic, CACHE_MAGIC));
        return STATUS_UNRECOGNIZED_MEDIA;
    }

    /*
     * Check version compatibility.
     */
    if (sb->version_major != CACHE_VERSION_MAJOR) {
        KdPrint(("CacheFlt: SuperblockRead — unsupported version %u.%u\n",
            sb->version_major, sb->version_minor));
        return STATUS_REVISION_MISMATCH;
    }

    KdPrint(("CacheFlt: Superblock OK — block_size=%u, total_blocks=%llu\n",
        sb->block_size, sb->total_cache_blocks));

    return STATUS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * SuperblockWrite
 *
 * Write the superblock back to sector 0. Used when initializing the cache
 * and on clean shutdown.
 *----------------------------------------------------------------------------*/
NTSTATUS
SuperblockWrite(
    _In_ CACHE_DISK_CONTEXT *ctx,
    _In_ CACHE_SUPERBLOCK   *sb)
{
    NTSTATUS          status;
    IO_STATUS_BLOCK   io_status;
    LARGE_INTEGER     byte_offset;

    if (ctx->cache_file_object == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    byte_offset.QuadPart = 0;

    status = ZwWriteFile(
        ctx->cache_file_object,
        NULL,
        NULL,
        NULL,
        &io_status,
        sb,
        sizeof(CACHE_SUPERBLOCK),
        &byte_offset,
        NULL);

    if (!NT_SUCCESS(status)) {
        KdPrint(("CacheFlt: SuperblockWrite failed 0x%08x\n", status));
    }

    return status;
}

/*----------------------------------------------------------------------------
 * SuperblockInitialize
 *
 * Populate a superblock structure with initial values for a fresh cache.
 * This is called once when setting up a new cache partition via the
 * user-mode manager. The manager then sends this down via IOCTL and we
 * write it to the SSD.
 *----------------------------------------------------------------------------*/
NTSTATUS
SuperblockInitialize(
    _In_    CACHE_DISK_CONTEXT *ctx,
    _Out_   CACHE_SUPERBLOCK   *sb,
    _In_    UINT64              source_disk_serial,
    _In_    UINT32              block_size,
    _In_    UINT64              total_blocks)
{
    UINT64  index_size_bytes;
    UINT64  index_size_sectors;

    RtlZeroMemory(sb, sizeof(CACHE_SUPERBLOCK));

    sb->magic             = CACHE_MAGIC;
    sb->version_major     = CACHE_VERSION_MAJOR;
    sb->version_minor     = CACHE_VERSION_MINOR;
    sb->block_size        = block_size;
    sb->total_cache_blocks = total_blocks;
    sb->source_disk_serial = source_disk_serial;
    sb->dirty_shutdown    = 0;

    /*
     * Calculate offsets:
     * - Index starts at sector 1 (right after superblock)
     * - Data starts after the index region
     *
     * Index size: total_blocks * sizeof(CACHE_BLOCK_ENTRY)
     * Align data start to 64KB boundary for performance.
     */
    index_size_bytes = total_blocks * sizeof(CACHE_BLOCK_ENTRY);
    index_size_sectors = (index_size_bytes + 511) / 512;  /* round up to sectors */

    /* Align data_offset to 128-sector (64KB) boundary */
    sb->index_offset = 1;  /* sector 1 */
    sb->data_offset  = ((1 + index_size_sectors + 127) / 128) * 128;

    UNREFERENCED_PARAMETER(ctx);

    KdPrint(("CacheFlt: SuperblockInitialized — index at sector %llu, data at sector %llu\n",
        sb->index_offset, sb->data_offset));

    return STATUS_SUCCESS;
}
