/*------------------------------------------------------------------------------
 * cache.h — Core cache data structures and global state
 *------------------------------------------------------------------------------
 */

#ifndef DRIVER_CACHE_H
#define DRIVER_CACHE_H

#include <ntddk.h>
#include <wdf.h>
#include "..\shared\protocol.h"

/*----------------------------------------------------------------------------
 * Per-disk cache state
 *
 * Each physical disk we accelerate has one of these, allocated as the
 * WDFDEVICE context for our filter device object.
 *----------------------------------------------------------------------------*/
typedef struct _CACHE_DISK_CONTEXT {

    /* --- Source (HDD) info --- */
    PDEVICE_OBJECT  source_physical_device;    /* underlying HDD device object */
    WDFIOTARGET     source_io_target;          /* KMDF target for source HDD */
    UINT64          source_disk_size_bytes;    /* total size of source disk */
    BOOLEAN         caching_active;            /* FALSE = pass-through only */

    /* --- Cache (SSD) info --- */
    PFILE_OBJECT    cache_file_object;         /* file object for raw partition */
    WDFIOTARGET     cache_io_target;           /* KMDF target for SSD reads */
    DEVICE_OBJECT  *cache_device_object;       /* for ZwReadFile/ZwWriteFile */
    UINT32          block_size;                /* e.g., 65536 (64KB) */
    UINT64          total_cache_blocks;        /* how many slots */

    /* --- In-memory block map --- */
    CACHE_BLOCK_ENTRY  *block_map;             /* array [total_cache_blocks] in non-paged pool */
    UINT64              cached_block_count;     /* number of VALID blocks */

    /* --- Locking --- */
    KSPIN_LOCK      block_map_lock;            /* protects block_map[] */

    /* --- CLOCK eviction hand --- */
    UINT64          lru_scan_cursor;           /* current clock-hand position */

    /* --- Work items for async population --- */
    IO_WORKITEM    *populate_work_item;        /* reusable work item */
    IO_WORKITEM    *index_sync_work_item;      /* periodic index flush to SSD */
    BOOLEAN          populate_in_progress;     /* prevents double-queue */

    /* --- Statistics --- */
    volatile LONGLONG  total_reads;
    volatile LONGLONG  cache_hits;
    volatile LONGLONG  cache_misses;
    volatile LONGLONG  bytes_read_total;
    volatile LONGLONG  bytes_read_from_cache;
    volatile LONGLONG  populations_queued;
    volatile LONGLONG  populations_completed;
    volatile LONGLONG  evictions;

    /* --- Pending populate list head (for race prevention) --- */
    LIST_ENTRY         pending_populations;
    KSPIN_LOCK         pending_lock;

    /* --- Global list linkage --- */
    LIST_ENTRY         global_list_entry;          /* linked into g_cache_disk_list */

} CACHE_DISK_CONTEXT;

/*
 * WDF_DECLARE_CONTEXT_TYPE_WITH_NAME — tells KMDF how to allocate and
 * retrieve our per-device context structure.
 */
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CACHE_DISK_CONTEXT, CacheDiskGetContext)

/*----------------------------------------------------------------------------
 * Global state — shared across all filter instances
 *----------------------------------------------------------------------------*/
extern LIST_ENTRY   g_cache_disk_list;      /* linked list of CACHE_DISK_CONTEXT */
extern KSPIN_LOCK   g_cache_disk_list_lock; /* protects g_cache_disk_list */
extern PDEVICE_OBJECT g_control_device;     /* named control device for IOCTLs */

/*----------------------------------------------------------------------------
 * Find a CACHE_DISK_CONTEXT by source disk device name
 *----------------------------------------------------------------------------*/
CACHE_DISK_CONTEXT*
CacheDiskLookupBySourceName(
    _In_ PCWSTR source_name
);

/*----------------------------------------------------------------------------
 * Per-populate-work-item context (allocated for each missed block)
 *----------------------------------------------------------------------------*/
typedef struct _POPULATE_WORK_ITEM_CONTEXT {
    LIST_ENTRY      list_entry;
    UINT64          source_lba;          /* source block to read */
    UINT64          cache_slot;          /* destination slot in cache */
    CACHE_DISK_CONTEXT *disk_ctx;        /* back-pointer */
} POPULATE_WORK_ITEM_CONTEXT;

#endif /* DRIVER_CACHE_H */
