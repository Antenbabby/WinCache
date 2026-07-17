/*------------------------------------------------------------------------------
 * stats.c — Statistics tracking helpers
 *
 * Uses Interlocked* operations for lock-free incrementing.
 *------------------------------------------------------------------------------
 */

#include <ntddk.h>
#include <wdf.h>
#include "cacheflt.h"

/*----------------------------------------------------------------------------
 * StatsRecordHit
 *
 * Called when a cache lookup succeeds and we serve data from SSD.
 *----------------------------------------------------------------------------*/
VOID
StatsRecordHit(
    _In_ CACHE_DISK_CONTEXT *ctx,
    _In_ ULONG               bytes_served)
{
    InterlockedIncrement64(&ctx->total_reads);
    InterlockedIncrement64(&ctx->cache_hits);
    InterlockedAdd64(&ctx->bytes_read_total, (LONGLONG)bytes_served);
    InterlockedAdd64(&ctx->bytes_read_from_cache, (LONGLONG)bytes_served);
}

/*----------------------------------------------------------------------------
 * StatsRecordMiss
 *
 * Called when a read is not found in cache and must go to HDD.
 *----------------------------------------------------------------------------*/
VOID
StatsRecordMiss(
    _In_ CACHE_DISK_CONTEXT *ctx,
    _In_ ULONG               bytes_requested)
{
    InterlockedIncrement64(&ctx->total_reads);
    InterlockedIncrement64(&ctx->cache_misses);
    InterlockedAdd64(&ctx->bytes_read_total, (LONGLONG)bytes_requested);
}

/*----------------------------------------------------------------------------
 * StatsRecordPopulationComplete
 *
 * Called when an async cache population work item finishes.
 *----------------------------------------------------------------------------*/
VOID
StatsRecordPopulationComplete(
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    InterlockedIncrement64(&ctx->populations_completed);
}

/*----------------------------------------------------------------------------
 * StatsRecordEviction
 *
 * Called when a cache block is evicted to make room for new data.
 *----------------------------------------------------------------------------*/
VOID
StatsRecordEviction(
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    InterlockedIncrement64(&ctx->evictions);
}
