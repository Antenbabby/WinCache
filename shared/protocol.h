/*------------------------------------------------------------------------------
 * protocol.h — Shared definitions between kernel driver and user-mode manager
 *
 * This file is included by both cacheflt.sys (kernel) and cachectl.exe (user).
 *------------------------------------------------------------------------------
 */

#ifndef SHARED_PROTOCOL_H
#define SHARED_PROTOCOL_H

#ifdef _KERNEL_MODE
  #include <ntddk.h>
#else
  #include <windows.h>
#endif

/*----------------------------------------------------------------------------
 * Magic & Version
 *----------------------------------------------------------------------------*/
#define CACHE_MAGIC              0x4548434143ULL  /* "CACHE" in little-endian */
#define CACHE_VERSION_MAJOR      1
#define CACHE_VERSION_MINOR      0

/*----------------------------------------------------------------------------
 * Configurable defaults
 *----------------------------------------------------------------------------*/
#define CACHE_DEFAULT_BLOCK_SIZE 65536            /* 64 KB blocks */
#define CACHE_LINEAR_PROBE_MAX   8                /* max probes in hash lookup */
#define CACHE_LRU_SCAN_BATCH     64               /* blocks scanned per eviction pass */

/*----------------------------------------------------------------------------
 * Block entry flags
 *----------------------------------------------------------------------------*/
#define CACHE_FLAG_VALID         0x00000001       /* block holds valid data */
#define CACHE_FLAG_POPULATING    0x00000002       /* async fill in progress */
#define CACHE_FLAG_PINNED        0x00000004       /* never evict */
#define CACHE_FLAG_ACCESSED      0x00000008       /* CLOCK "referenced" bit */

#define CACHE_INVALID_INDEX      0xFFFFFFFFFFFFFFFFULL

/*----------------------------------------------------------------------------
 * Superblock — on-SSD sector 0 (512 bytes)
 *----------------------------------------------------------------------------*/
#pragma pack(push, 1)
typedef struct _CACHE_SUPERBLOCK {
    UINT64  magic;
    UINT32  version_major;
    UINT32  version_minor;
    UINT32  block_size;
    UINT32  reserved0;
    UINT64  total_cache_blocks;
    UINT64  index_offset;        /* sector where index region starts */
    UINT64  data_offset;         /* sector where data region starts */
    UINT64  source_disk_serial;
    UINT8   dirty_shutdown;
    UINT8   padding[447];        /* pad to 512 bytes */
} CACHE_SUPERBLOCK;
#pragma pack(pop)

/*----------------------------------------------------------------------------
 * Block Map Entry — one per cache slot (24 bytes)
 *----------------------------------------------------------------------------*/
#pragma pack(push, 1)
typedef struct _CACHE_BLOCK_ENTRY {
    UINT64  source_lba;          /* source disk starting LBA */
    UINT64  last_access;         /* KeQueryTickCount() / GetTickCount64() */
    UINT32  flags;               /* CACHE_FLAG_* */
    UINT32  access_count;        /* used for LFU scoring */
} CACHE_BLOCK_ENTRY;
#pragma pack(pop)

/*----------------------------------------------------------------------------
 * IOCTL codes
 *
 * We use FILE_DEVICE_UNKNOWN (0x22) as the device type and function codes
 * starting at 0x900 to avoid collisions with system-defined codes.
 *----------------------------------------------------------------------------*/
#define CACHE_IOCTL_TYPE          0x00000022  /* FILE_DEVICE_UNKNOWN */

#define IOCTL_CACHE_ATTACH        CTL_CODE(CACHE_IOCTL_TYPE, 0x900, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_CACHE_DETACH        CTL_CODE(CACHE_IOCTL_TYPE, 0x901, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_CACHE_GET_STATS     CTL_CODE(CACHE_IOCTL_TYPE, 0x902, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_CACHE_RESET_STATS   CTL_CODE(CACHE_IOCTL_TYPE, 0x903, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_CACHE_GET_CONFIG    CTL_CODE(CACHE_IOCTL_TYPE, 0x905, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_CACHE_PRELOAD_RANGE CTL_CODE(CACHE_IOCTL_TYPE, 0x907, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_CACHE_FLUSH         CTL_CODE(CACHE_IOCTL_TYPE, 0x908, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

/*----------------------------------------------------------------------------
 * IOCTL parameter structures
 *----------------------------------------------------------------------------*/
#pragma pack(push, 1)

/* All IOCTL inputs start with this common header so the control
 * device dispatch can identify which disk to operate on. */
typedef struct _CACHE_IOCTL_HEADER {
    WCHAR   source_disk[256];             /* e.g. L"\\Device\\Harddisk2\\DR2" */
} CACHE_IOCTL_HEADER;

/* IOCTL_CACHE_ATTACH — input */
typedef struct _CACHE_ATTACH_INPUT {
    CACHE_IOCTL_HEADER header;            /* source_disk = HDD to accelerate */
    WCHAR   cache_partition[256];         /* e.g. L"\\Device\\Harddisk3\\Partition1" */
    UINT32  block_size;                   /* 0 = use default (64KB) */
    UINT32  flags;                        /* reserved */
} CACHE_ATTACH_INPUT;

/* Simple IOCTLs that need only the header to identify the disk */
typedef CACHE_IOCTL_HEADER CACHE_STATS_INPUT;
typedef CACHE_IOCTL_HEADER CACHE_DETACH_INPUT;
typedef CACHE_IOCTL_HEADER CACHE_RESET_STATS_INPUT;
typedef CACHE_IOCTL_HEADER CACHE_CONFIG_INPUT;
typedef CACHE_IOCTL_HEADER CACHE_FLUSH_INPUT;

/* IOCTL_CACHE_GET_STATS — output */
typedef struct _CACHE_STATS_OUTPUT {
    UINT64  total_reads;
    UINT64  cache_hits;
    UINT64  cache_misses;
    UINT64  bytes_read_total;
    UINT64  bytes_read_from_cache;
    UINT64  blocks_cached;
    UINT64  blocks_total;
    UINT64  populations_queued;
    UINT64  populations_completed;
    UINT64  evictions;
    UINT64  hit_rate_percent_x100;        /* hit rate * 100 (e.g. 8534 = 85.34%) */
} CACHE_STATS_OUTPUT;

/* IOCTL_CACHE_GET_CONFIG — output */
typedef struct _CACHE_CONFIG_OUTPUT {
    UINT32  block_size;
    UINT32  is_active;
    UINT64  total_cache_blocks;
    UINT64  cached_blocks;
    UINT64  source_disk_serial;
    WCHAR   cache_partition_name[256];
} CACHE_CONFIG_OUTPUT;

/* IOCTL_CACHE_PRELOAD_RANGE — input */
typedef struct _CACHE_PRELOAD_INPUT {
    CACHE_IOCTL_HEADER header;            /* source_disk */
    UINT64  start_lba;
    UINT64  end_lba;                      /* inclusive */
} CACHE_PRELOAD_INPUT;

#pragma pack(pop)

#endif /* SHARED_PROTOCOL_H */
