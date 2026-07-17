/*------------------------------------------------------------------------------
 * cacheflt.h — Function declarations for the cache filter driver
 *------------------------------------------------------------------------------
 */

#ifndef DRIVER_CACHEFLT_H
#define DRIVER_CACHEFLT_H

#include <ntddk.h>
#include <wdf.h>
#include "cache.h"

/*----------------------------------------------------------------------------
 * Globals — stored in driver context (retrieved via WdfDriver WDFNOBJECT)
 *----------------------------------------------------------------------------*/
typedef struct _CACHE_DRIVER_GLOBALS {
    PDRIVER_OBJECT   driver_object;
    UNICODE_STRING   registry_path;
    WDFDRIVER        wdf_driver;
} CACHE_DRIVER_GLOBALS;

/*----------------------------------------------------------------------------
 * cacheflt.c — Driver entry & lifecycle
 *----------------------------------------------------------------------------*/
DRIVER_INITIALIZE         DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD CacheFilterEvtDeviceAdd;
EVT_WDF_DRIVER_UNLOAD     CacheFilterEvtDriverUnload;
EVT_WDF_DEVICE_CONTEXT_CLEANUP CacheFilterEvtDeviceContextCleanup;

/*----------------------------------------------------------------------------
 * io.c — I/O dispatch (read, write)
 *----------------------------------------------------------------------------*/
EVT_WDF_IO_QUEUE_IO_READ          CacheFilterEvtIoRead;
EVT_WDF_IO_QUEUE_IO_WRITE         CacheFilterEvtIoWrite;
EVT_WDF_REQUEST_COMPLETION_ROUTINE CacheFilterReadCompletionRoutine;

NTSTATUS
CacheFilterForwardRead(
    WDFREQUEST  request,
    CACHE_DISK_CONTEXT *ctx
);

NTSTATUS
CacheFilterServiceFromCache(
    WDFREQUEST  request,
    CACHE_DISK_CONTEXT *ctx,
    UINT64      cache_slot
);

/*----------------------------------------------------------------------------
 * ioctl.c — Device control (IOCTL dispatch)
 *----------------------------------------------------------------------------*/
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL CacheFilterEvtIoDeviceControl;

NTSTATUS
CacheFilterIoctlAttach(
    CACHE_DISK_CONTEXT *ctx,
    CACHE_ATTACH_INPUT *input,
    ULONG               input_len
);

NTSTATUS
CacheFilterIoctlDetach(
    CACHE_DISK_CONTEXT *ctx
);

NTSTATUS
CacheFilterIoctlGetStats(
    CACHE_DISK_CONTEXT  *ctx,
    CACHE_STATS_OUTPUT  *output,
    ULONG                output_len
);

NTSTATUS
CacheFilterIoctlResetStats(
    CACHE_DISK_CONTEXT *ctx
);

NTSTATUS
CacheFilterIoctlGetConfig(
    CACHE_DISK_CONTEXT    *ctx,
    CACHE_CONFIG_OUTPUT   *output,
    ULONG                  output_len
);

NTSTATUS
CacheFilterIoctlPreloadRange(
    CACHE_DISK_CONTEXT  *ctx,
    CACHE_PRELOAD_INPUT *input,
    ULONG                input_len
);

NTSTATUS
CacheFilterIoctlFlush(
    CACHE_DISK_CONTEXT *ctx
);

/*----------------------------------------------------------------------------
 * cache.c — Cache logic (lookup, populate, evict)
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheInit(
    CACHE_DISK_CONTEXT *ctx
);

VOID
CacheTeardown(
    CACHE_DISK_CONTEXT *ctx
);

UINT64
CacheLookup(
    CACHE_DISK_CONTEXT *ctx,
    UINT64              source_lba
);

VOID
CacheMarkAccessed(
    CACHE_DISK_CONTEXT *ctx,
    UINT64              slot
);

NTSTATUS
CachePopulateAsync(
    CACHE_DISK_CONTEXT *ctx,
    UINT64              source_lba
);

UINT64
CacheSelectEvictionSlot(
    CACHE_DISK_CONTEXT *ctx
);

/*----------------------------------------------------------------------------
 * superblock.c — Superblock read/write/validate
 *----------------------------------------------------------------------------*/
NTSTATUS
SuperblockRead(
    CACHE_DISK_CONTEXT *ctx,
    CACHE_SUPERBLOCK   *sb
);

NTSTATUS
SuperblockWrite(
    CACHE_DISK_CONTEXT *ctx,
    CACHE_SUPERBLOCK   *sb
);

NTSTATUS
SuperblockInitialize(
    CACHE_DISK_CONTEXT *ctx,
    CACHE_SUPERBLOCK   *sb,
    UINT64              source_disk_serial,
    UINT32              block_size,
    UINT64              total_blocks
);

/*----------------------------------------------------------------------------
 * index.c — Block map load/store to SSD
 *----------------------------------------------------------------------------*/
NTSTATUS
IndexLoadFromSsd(
    CACHE_DISK_CONTEXT *ctx
);

NTSTATUS
IndexFlushToSsd(
    CACHE_DISK_CONTEXT *ctx
);

/*----------------------------------------------------------------------------
 * lru.c — CLOCK eviction algorithm
 *----------------------------------------------------------------------------*/
UINT64
LruClockFindVictim(
    CACHE_DISK_CONTEXT *ctx
);

VOID
LruClockResetCursor(
    CACHE_DISK_CONTEXT *ctx
);

/*----------------------------------------------------------------------------
 * attach.c — Attach/detach logic
 *----------------------------------------------------------------------------*/
NTSTATUS
CacheAttachToDisk(
    CACHE_DISK_CONTEXT *ctx,
    CACHE_ATTACH_INPUT *input
);

NTSTATUS
CacheDetachFromDisk(
    CACHE_DISK_CONTEXT *ctx
);

/*----------------------------------------------------------------------------
 * stats.c — Statistics helpers
 *----------------------------------------------------------------------------*/
VOID
StatsRecordHit(
    CACHE_DISK_CONTEXT *ctx,
    ULONG               bytes_served
);

VOID
StatsRecordMiss(
    CACHE_DISK_CONTEXT *ctx,
    ULONG               bytes_requested
);

VOID
StatsRecordPopulationComplete(
    CACHE_DISK_CONTEXT *ctx
);

VOID
StatsRecordEviction(
    CACHE_DISK_CONTEXT *ctx
);

#endif /* DRIVER_CACHEFLT_H */
