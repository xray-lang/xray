/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregion.c - Region Mark-Region memory allocator
 *
 * Allocation: bump pointer within "holes" (contiguous free lines).
 * Reclamation: line-granularity (128B) via mark bitmap after GC sweep.
 * Blocks with zero live lines are returned to the free pool.
 */

#include "xregion.h"
#include "../../base/xchecks.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========== Platform-Specific Aligned Allocation ========== */

static char *alloc_aligned_block(void) {
    void *ptr = NULL;
#if defined(XR_OS_WINDOWS)
    ptr = _aligned_malloc(XR_REGION_BLOCK_SIZE, XR_REGION_BLOCK_SIZE);
#else
    if (posix_memalign(&ptr, XR_REGION_BLOCK_SIZE, XR_REGION_BLOCK_SIZE) != 0) {
        return NULL;
    }
#endif
    return (char *) ptr;
}

static void free_aligned_block(char *data) {
#if defined(XR_OS_WINDOWS)
    _aligned_free(data);
#else
    // posix_memalign paired with libc free is the documented contract.
    free(data);  // xr:allow-raw-alloc
#endif
}

/* ========== Two-Level Block Cache ========== */
/*
 * L1: per-Worker array (XrP.block_cache[8]), lock-free (single-thread access).
 * L2: per-isolate stack on XrSystemHeap (mutex protected), reached through the
 *     owning heap's `sys_heap`.
 *
 * L1 miss → L2 → fresh allocation.
 * L1 overflow → L2 → free_aligned_block if L2 full.
 * Worker exit flushes L1 → L2 (see xr_region_flush_block_cache).
 *
 * The L2 pool was previously a process-wide static, which crossed isolate
 * boundaries and only released cached blocks at process exit (atexit). It is
 * now owned by XrSystemHeap so each isolate recycles its own blocks and
 * xr_sysheap_destroy returns them to the OS.
 */

#include "../../os/os_thread.h"
#include "../../coro/xworker.h"
#include "xsystem_heap.h"

void xr_region_free_raw_block(void *block) {
    free_aligned_block((char *) block);
}

// Pop: try L1 (per-worker), then L2 (per-isolate via heap->sys_heap)
static XrRegionBlock *block_cache_pop(XrRegionHeap *heap) {
    XrWorker *w = xr_current_worker();
    if (w && w->p.block_cache_count > 0) {
        w->p.block_cache_count--;
        return (XrRegionBlock *) w->p.block_cache[w->p.block_cache_count];
    }
    return heap ? (XrRegionBlock *) xr_sysheap_block_pool_pop(heap->sys_heap) : NULL;
}

// Push: try L1 (per-worker), then L2 (per-isolate), then return to the OS
static void block_cache_push(XrRegionHeap *heap, XrRegionBlock *block) {
    XrWorker *w = xr_current_worker();
    if (w && w->p.block_cache_count < XR_BLOCK_CACHE_L1_MAX) {
        w->p.block_cache[w->p.block_cache_count++] = block;
        return;
    }
    if (!heap || !xr_sysheap_block_pool_push(heap->sys_heap, block)) {
        free_aligned_block((char *) block);
    }
}

// Flush a per-worker L1 block cache to the isolate's L2 pool.
// Blocks that don't fit are returned to the OS.
void xr_region_flush_block_cache(struct XrSystemHeap *sys_heap, void *block_cache[], int *count) {
    XR_DCHECK(block_cache != NULL, "flush_block_cache: NULL cache array");
    XR_DCHECK(count != NULL, "flush_block_cache: NULL count");
    for (int i = 0; i < *count; i++) {
        if (!sys_heap || !xr_sysheap_block_pool_push(sys_heap, block_cache[i])) {
            free_aligned_block((char *) block_cache[i]);
        }
        block_cache[i] = NULL;
    }
    *count = 0;
}

/* ========== Block Management ========== */

static XrRegionBlock *block_new(XrRegionHeap *heap) {
    // Try the block caches first, then allocate fresh
    XrRegionBlock *block = block_cache_pop(heap);
    if (!block) {
        char *data = alloc_aligned_block();
        if (!data)
            return NULL;
        block = (XrRegionBlock *) data;
    }

    // Zero the metadata line (line 0); object area starts at line 1.
    memset(block, 0, XR_REGION_LINE_SIZE);

    XR_DCHECK(((uintptr_t) block & (XR_REGION_BLOCK_SIZE - 1)) == 0,
              "block_new: block not aligned to BLOCK_SIZE");
    return block;
}

static void block_free(XrRegionHeap *heap, XrRegionBlock *block) {
    if (!block)
        return;
    XR_DCHECK(((uintptr_t) block & (XR_REGION_BLOCK_SIZE - 1)) == 0,
              "block_free: unaligned block pointer");
    if ((uintptr_t) block & (XR_REGION_BLOCK_SIZE - 1)) {
        return;
    }
    block_cache_push(heap, block);
}

static void free_block_list(XrRegionHeap *heap, XrRegionBlock *list) {
    while (list) {
        if ((uintptr_t) list & (XR_REGION_BLOCK_SIZE - 1)) {
            break;
        }
        XrRegionBlock *next = list->next;
        block_free(heap, list);
        list = next;
    }
}

/* ========== Lifecycle ========== */

void xr_region_init(XrRegionHeap *heap) {
    XR_DCHECK(heap != NULL, "region_init: NULL heap");
    memset(heap, 0, sizeof(XrRegionHeap));
}

void xr_region_destroy(XrRegionHeap *heap) {
    XR_DCHECK(heap != NULL, "region_destroy: NULL heap");
    if (heap->current_block) {
        block_free(heap, heap->current_block);
        heap->current_block = NULL;
    }
    free_block_list(heap, heap->full_blocks);
    free_block_list(heap, heap->free_blocks);

    heap->full_blocks = NULL;
    heap->free_blocks = NULL;
    heap->cursor = NULL;
    heap->limit = NULL;
    heap->total_blocks = 0;
    heap->total_block_bytes = 0;
}

void xr_region_reset(XrRegionHeap *heap) {
    XR_DCHECK(heap != NULL, "region_reset: NULL heap");
    xr_region_destroy(heap);
    xr_region_init(heap);
}

/* ========== Allocation — Slow Path Helpers ========== */

// Retire the current block to full_blocks (its bump space is used up).
static void retire_current_block(XrRegionHeap *heap) {
    if (heap->current_block) {
        heap->current_block->next = heap->full_blocks;
        heap->full_blocks = heap->current_block;
        heap->current_block = NULL;
    }
}

// Reset a free block and set it as current
static void activate_block(XrRegionHeap *heap, XrRegionBlock *block) {
    XR_DCHECK(heap != NULL, "activate_block: NULL heap");
    XR_DCHECK(block != NULL, "activate_block: NULL block");
    /* Reused block starts a fresh allocation generation: the old objects
     * were reclaimed (whole-block reclaim) or the block came from the cache.
     * alloc_count must count only the new generation, or the whole-block
     * reclaim invariant (dead-slot tally == alloc_count) would be wrong. */
    block->alloc_count = 0;
    block->alloc_bytes = 0;
    block->reclaim_dead_count = 0;

    heap->cursor = (char *) block + (size_t) XR_REGION_FIRST_LINE * XR_REGION_LINE_SIZE;
    heap->limit = (char *) block + XR_REGION_BLOCK_SIZE;
    heap->current_block = block;
    XR_DCHECK(heap->cursor <= heap->limit, "activate_block: cursor > limit");
}

static bool try_free_block(XrRegionHeap *heap) {
    if (!heap->free_blocks)
        return false;
    XrRegionBlock *block = heap->free_blocks;
    heap->free_blocks = block->next;
    block->next = NULL;
    activate_block(heap, block);
    return true;
}

static bool alloc_new_block(XrRegionHeap *heap) {
    XR_DCHECK(heap != NULL, "alloc_new_block: NULL heap");
    // Retire current block first
    if (heap->current_block) {
        heap->current_block->next = heap->full_blocks;
        heap->full_blocks = heap->current_block;
        heap->current_block = NULL;
    }

    XrRegionBlock *block = block_new(heap);
    if (!block)
        return false;

    heap->current_block = block;
    heap->cursor = (char *) block + (size_t) XR_REGION_FIRST_LINE * XR_REGION_LINE_SIZE;
    heap->limit = (char *) block + XR_REGION_BLOCK_SIZE;
    heap->total_blocks++;
    heap->total_block_bytes += XR_REGION_BLOCK_SIZE;
    XR_DCHECK(heap->cursor < heap->limit, "alloc_new_block: cursor >= limit");
    return true;
}

/* ========== Allocation ========== */

void *xr_region_alloc_slow(XrRegionHeap *heap, size_t size) {
    XR_DCHECK(heap != NULL, "region_alloc_slow: NULL heap");
    XR_DCHECK(size > 0, "region_alloc_slow: zero size");

    // Current block's bump space is exhausted: retire it to full_blocks
    // before pulling a fresh block (whole-block reclaim refills free_blocks).
    // Under pure RC there is no intra-block hole reuse — dead small objects
    // return to the per-coroutine size-class freelist, not to the block.
    retire_current_block(heap);

    // Try a reclaimed empty block, else allocate a new one.
    if (!try_free_block(heap) && !alloc_new_block(heap))
        return NULL;

    char *result = heap->cursor;
    heap->cursor = result + size;
    XR_DCHECK(heap->cursor <= heap->limit, "alloc_slow: cursor > limit after alloc");
    return result;
}

// xr_region_alloc() is now static inline in xregion.h

/* ========== Debug ========== */

void xr_region_get_stats(XrRegionHeap *heap, XrRegionStats *stats) {
    XR_DCHECK(heap != NULL, "region_get_stats: NULL heap");
    XR_DCHECK(stats != NULL, "region_get_stats: NULL stats");
    memset(stats, 0, sizeof(XrRegionStats));

    for (XrRegionBlock *b = heap->full_blocks; b; b = b->next) {
        stats->total_blocks++;
        stats->full_blocks++;
    }
    for (XrRegionBlock *b = heap->free_blocks; b; b = b->next) {
        stats->total_blocks++;
        stats->free_blocks++;
    }
    if (heap->current_block)
        stats->total_blocks++;

    stats->total_bytes = stats->total_blocks * XR_REGION_BLOCK_SIZE;
}
