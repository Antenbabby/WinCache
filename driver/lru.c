/*------------------------------------------------------------------------------
 * lru.c — CLOCK eviction algorithm (approximate LRU)
 *
 * We use the CLOCK (second-chance) algorithm, which is simpler and cheaper
 * than true LRU while achieving similar hit rates for most workloads.
 *
 * How it works:
 * 1. Each block has a CACHE_FLAG_ACCESSED bit (the "reference bit")
 * 2. On cache hit: set FLAG_ACCESSED + update last_access timestamp
 * 3. On eviction: scan from lru_scan_cursor
 *    a. If FLAG_ACCESSED is set: clear it, skip to next block
 *    b. If FLAG_ACCESSED is clear: this is the victim (evict it)
 * 4. The last_access timestamp is a tiebreaker when all blocks are
 *    marked ACCESSED (rare in steady state)
 *
 * This is O(1) per eviction (amortized) vs O(N) for true LRU.
 *------------------------------------------------------------------------------
 */

#include <ntddk.h>
#include <wdf.h>
#include "cacheflt.h"
#include "..\shared\protocol.h"

/*----------------------------------------------------------------------------
 * LruClockFindVictim
 *
 * Scan the block map starting from lru_scan_cursor, looking for a block
 * to evict. Uses the CLOCK second-chance algorithm.
 *
 * Must be called with block_map_lock held.
 *
 * Returns: slot index to evict, or CACHE_INVALID_INDEX if nothing available.
 *----------------------------------------------------------------------------*/
UINT64
LruClockFindVictim(
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    UINT64  start_cursor;
    UINT64  scanned;
    UINT64  oldest_slot;
    UINT64  oldest_time;
    UINT64  current_slot;
    UINT64  current_time;

    if (ctx->total_cache_blocks == 0 || ctx->block_map == NULL) {
        return CACHE_INVALID_INDEX;
    }

    start_cursor = ctx->lru_scan_cursor;
    scanned      = 0;
    oldest_slot  = CACHE_INVALID_INDEX;
    oldest_time  = ~0ULL;  /* max value — anything is older */

    for (scanned = 0; scanned < ctx->total_cache_blocks; scanned++) {
        current_slot = (start_cursor + scanned) % ctx->total_cache_blocks;

        /*
         * Skip slots that are:
         * - Not valid (nothing to evict)
         * - Currently populating (don't touch in-flight I/O)
         * - Pinned (user marked as "never evict")
         */
        if (!(ctx->block_map[current_slot].flags & CACHE_FLAG_VALID)) {
            continue;
        }

        if ((ctx->block_map[current_slot].flags & CACHE_FLAG_POPULATING)) {
            continue;
        }

        if ((ctx->block_map[current_slot].flags & CACHE_FLAG_PINNED)) {
            continue;
        }

        /*
         * Track the oldest block seen (by last_access) as a fallback
         * in case all blocks have the ACCESSED flag.
         */
        current_time = ctx->block_map[current_slot].last_access;
        if (current_time <= oldest_time) {
            oldest_time = current_time;
            oldest_slot = current_slot;
        }

        /*
         * CLOCK second-chance: if the accessed bit is set, clear it and
         * give the block another round (second chance).
         */
        if (ctx->block_map[current_slot].flags & CACHE_FLAG_ACCESSED) {
            ctx->block_map[current_slot].flags &= ~CACHE_FLAG_ACCESSED;
            continue;
        }

        /*
         * Found a victim! Clear its valid flag and mark as evicting.
         */
        ctx->block_map[current_slot].flags &= ~CACHE_FLAG_VALID;
        ctx->cached_block_count--;

        /* Advance cursor past this slot for next time */
        ctx->lru_scan_cursor = (current_slot + 1) % ctx->total_cache_blocks;

        StatsRecordEviction(ctx);
        return current_slot;
    }

    /*
     * All blocks were either invalid or had ACCESSED set.
     * Use the oldest block found during the scan as fallback.
     */
    if (oldest_slot != CACHE_INVALID_INDEX) {
        ctx->block_map[oldest_slot].flags &= ~(CACHE_FLAG_VALID | CACHE_FLAG_ACCESSED);
        ctx->cached_block_count--;

        /* Update cursor for next eviction */
        ctx->lru_scan_cursor = (oldest_slot + 1) % ctx->total_cache_blocks;

        StatsRecordEviction(ctx);
        return oldest_slot;
    }

    /*
     * No evictable blocks found (all slots are pinned or empty).
     */
    return CACHE_INVALID_INDEX;
}

/*----------------------------------------------------------------------------
 * LruClockResetCursor
 *
 * Reset the clock hand back to the start of the block map. Called when
 * the cache is first initialized or after a full reset.
 *----------------------------------------------------------------------------*/
VOID
LruClockResetCursor(
    _In_ CACHE_DISK_CONTEXT *ctx)
{
    ctx->lru_scan_cursor = 0;
}
