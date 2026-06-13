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

    // Common init: zero metadata line, set non-zero fields
    memset(block, 0, XR_REGION_LINE_SIZE);
    block->alloc_marks[0] = 1ULL;  // line 0 reserved for metadata
    block->next_scan_line = XR_REGION_FIRST_LINE;
    block->is_young = 1;

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

/* ========== Hole Finding ========== */

/*
 * Scan alloc_marks starting from `start_line` to find the next contiguous
 * run of unmarked lines (a "hole").
 * Returns true if found, sets *hole_start and *hole_end (exclusive).
 */
static bool find_next_hole(XrRegionBlock *block, int start_line, int *hole_start, int *hole_end) {
    XR_DCHECK(block != NULL, "find_next_hole: NULL block");
    XR_DCHECK(start_line >= XR_REGION_FIRST_LINE && start_line <= XR_REGION_LINES,
              "find_next_hole: start_line out of range");
    XR_DCHECK(hole_start != NULL && hole_end != NULL, "find_next_hole: NULL output");
    // Build "free lines" bitmap (invert alloc_marks)
    uint64_t free0 = ~block->alloc_marks[0];
    uint64_t free1 = ~block->alloc_marks[1];

    // Mask out lines before start_line
    if (start_line < 64) {
        free0 &= ~((1ULL << start_line) - 1);
    } else {
        free0 = 0;
        if (start_line < 128)
            free1 &= ~((1ULL << (start_line - 64)) - 1);
        else
            free1 = 0;
    }

    // Find first free line
    int first_free;
    if (free0) {
        first_free = __builtin_ctzll(free0);
    } else if (free1) {
        first_free = 64 + __builtin_ctzll(free1);
    } else {
        return false;
    }

    *hole_start = first_free;

    // Find end of hole: first marked line after hole_start
    // Build "marked" bitmap from hole_start onwards
    uint64_t mark0 = block->alloc_marks[0];
    uint64_t mark1 = block->alloc_marks[1];

    if (first_free < 64) {
        mark0 &= ~((1ULL << first_free) - 1);  // mask bits below first_free
        if (mark0) {
            *hole_end = __builtin_ctzll(mark0);
            return true;
        }
        // No marked line in word 0; check word 1
        if (mark1) {
            *hole_end = 64 + __builtin_ctzll(mark1);
            return true;
        }
    } else {
        mark1 &= ~((1ULL << (first_free - 64)) - 1);
        if (mark1) {
            *hole_end = 64 + __builtin_ctzll(mark1);
            return true;
        }
    }

    *hole_end = XR_REGION_LINES;  // hole extends to end of block
    return true;
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
    free_block_list(heap, heap->recycle_blocks);
    free_block_list(heap, heap->free_blocks);
    free_block_list(heap, heap->old_blocks);

    heap->full_blocks = NULL;
    heap->recycle_blocks = NULL;
    heap->free_blocks = NULL;
    heap->old_blocks = NULL;
    heap->cursor = NULL;
    heap->limit = NULL;
    heap->total_blocks = 0;
    heap->total_block_bytes = 0;
    heap->old_block_count = 0;
}

void xr_region_reset(XrRegionHeap *heap) {
    XR_DCHECK(heap != NULL, "region_reset: NULL heap");
    xr_region_destroy(heap);
    xr_region_init(heap);
}

/* ========== Allocation — Slow Path Helpers ========== */

// Try to find a suitable hole in the current block
static bool try_current_block_hole(XrRegionHeap *heap, size_t size) {
    XR_DCHECK(heap != NULL, "try_current_block_hole: NULL heap");
    XR_DCHECK(size > 0, "try_current_block_hole: zero size");
    XrRegionBlock *block = heap->current_block;
    if (!block)
        return false;

    int hole_start, hole_end;
    while (find_next_hole(block, block->next_scan_line, &hole_start, &hole_end)) {
        size_t hole_size = (size_t) (hole_end - hole_start) * XR_REGION_LINE_SIZE;
        block->next_scan_line = (uint8_t) hole_end;
        if (hole_size >= size) {
            heap->cursor = (char *) block + (size_t) hole_start * XR_REGION_LINE_SIZE;
            heap->limit = (char *) block + (size_t) hole_end * XR_REGION_LINE_SIZE;
            heap->mark_cursor = heap->cursor;
            return true;
        }
    }
    return false;
}

// Move current_block to full_blocks, then try recycle list
static bool try_recycle_blocks(XrRegionHeap *heap, size_t size) {
    XR_DCHECK(heap != NULL, "try_recycle_blocks: NULL heap");
    // Retire current block
    if (heap->current_block) {
        heap->current_block->next = heap->full_blocks;
        heap->full_blocks = heap->current_block;
        heap->current_block = NULL;
    }

    while (heap->recycle_blocks) {
        XrRegionBlock *block = heap->recycle_blocks;
        heap->recycle_blocks = block->next;
        block->next = NULL;
        block->next_scan_line = XR_REGION_FIRST_LINE;
        heap->current_block = block;

        if (try_current_block_hole(heap, size)) {
            return true;
        }
        // No suitable hole, retire to full
        block->next = heap->full_blocks;
        heap->full_blocks = block;
        heap->current_block = NULL;
    }
    return false;
}

// Reset a free block and set it as current
static void activate_block(XrRegionHeap *heap, XrRegionBlock *block) {
    XR_DCHECK(heap != NULL, "activate_block: NULL heap");
    XR_DCHECK(block != NULL, "activate_block: NULL block");
    block->alloc_marks[0] = 1ULL;  // Line 0 reserved
    block->alloc_marks[1] = 0;
    block->next_scan_line = XR_REGION_FIRST_LINE;
    /* Reused block starts a fresh allocation generation: the old objects
     * were reclaimed (whole-block reclaim) or the block came from the cache.
     * alloc_count must count only the new generation, or the whole-block
     * reclaim invariant (dead-slot tally == alloc_count) would be wrong. */
    block->alloc_count = 0;
    block->alloc_bytes = 0;
    block->reclaim_dead_count = 0;

    heap->cursor = (char *) block + (size_t) XR_REGION_FIRST_LINE * XR_REGION_LINE_SIZE;
    heap->limit = (char *) block + XR_REGION_BLOCK_SIZE;
    heap->mark_cursor = heap->cursor;
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
    heap->mark_cursor = heap->cursor;
    heap->total_blocks++;
    heap->total_block_bytes += XR_REGION_BLOCK_SIZE;
    XR_DCHECK(heap->cursor < heap->limit, "alloc_new_block: cursor >= limit");
    return true;
}

/* ========== Allocation ========== */

void *xr_region_alloc_slow(XrRegionHeap *heap, size_t size) {
    XR_DCHECK(heap != NULL, "region_alloc_slow: NULL heap");
    XR_DCHECK(size > 0, "region_alloc_slow: zero size");
    // Flush deferred alloc_marks before hole-scanning
    xr_region_flush_marks(heap);

    // 1. Try remaining holes in current block
    if (try_current_block_hole(heap, size)) {
        char *result = heap->cursor;
        heap->cursor = result + size;
        return result;
    }

    // 2. Try recycle blocks
    if (try_recycle_blocks(heap, size)) {
        char *result = heap->cursor;
        heap->cursor = result + size;
        return result;
    }

    // 3. Try free blocks
    if (try_free_block(heap)) {
        char *result = heap->cursor;
        heap->cursor = result + size;
        return result;
    }

    // 4. Allocate new block
    if (!alloc_new_block(heap))
        return NULL;
    char *result = heap->cursor;
    heap->cursor = result + size;
    XR_DCHECK(heap->cursor <= heap->limit, "alloc_slow: cursor > limit after alloc");
    return result;
}

// xr_region_alloc() is now static inline in xregion.h

/* ========== GC Integration (Single Bitmap) ========== */

void xr_region_mark_alloc_lines(void *obj_ptr, size_t obj_size) {
    if (!obj_ptr || obj_size == 0)
        return;
    XrRegionBlock *block = XR_REGION_BLOCK_FROM_PTR(obj_ptr);
    char *block_data = (char *) block;
    uintptr_t base = (uintptr_t) block_data;
    uintptr_t start = (uintptr_t) obj_ptr;
    uintptr_t end = start + obj_size - 1;
    int first_line = (int) ((start - base) / XR_REGION_LINE_SIZE);
    int last_line = (int) ((end - base) / XR_REGION_LINE_SIZE);
    XR_DCHECK(first_line >= 0 && first_line < XR_REGION_LINES,
              "mark_alloc_lines: first_line out of range");
    XR_DCHECK(last_line >= 0 && last_line < XR_REGION_LINES,
              "mark_alloc_lines: last_line out of range");
    for (int l = first_line; l <= last_line; l++) {
        XR_REGION_LINE_SET(block->alloc_marks, l);
    }
}

/*
 * Count live lines in a block (excluding the reserved line 0).
 * Uses popcount on the bitmap words with line-0 bit masked out.
 */
int xr_region_count_live_lines(XrRegionBlock *block) {
    XR_DCHECK(block != NULL, "count_live_lines: NULL block");
    uint64_t w0 = block->alloc_marks[0] & ~1ULL;  // Exclude bit 0 (line 0)
    uint64_t w1 = block->alloc_marks[1];

#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(w0) + __builtin_popcountll(w1);
#else
    int count = 0;
    while (w0) {
        count++;
        w0 &= w0 - 1;
    }
    while (w1) {
        count++;
        w1 &= w1 - 1;
    }
    return count;
#endif
}

/* ========== Debug ========== */

void xr_region_get_stats(XrRegionHeap *heap, XrRegionStats *stats) {
    XR_DCHECK(heap != NULL, "region_get_stats: NULL heap");
    XR_DCHECK(stats != NULL, "region_get_stats: NULL stats");
    memset(stats, 0, sizeof(XrRegionStats));

// Helper: accumulate stats for a block list
#define SCAN_LIST(list, counter)                                                                   \
    do {                                                                                           \
        for (XrRegionBlock *b = (list); b; b = b->next) {                                          \
            stats->total_blocks++;                                                                 \
            stats->counter++;                                                                      \
            int live = xr_region_count_live_lines(b);                                              \
            stats->live_lines += (size_t) live;                                                    \
            stats->free_lines += (size_t) (XR_REGION_USABLE_LINES - live);                         \
        }                                                                                          \
    } while (0)

    SCAN_LIST(heap->full_blocks, full_blocks);
    SCAN_LIST(heap->recycle_blocks, recycle_blocks);

    // Free blocks: all usable lines are free
    for (XrRegionBlock *b = heap->free_blocks; b; b = b->next) {
        stats->total_blocks++;
        stats->free_blocks++;
        stats->free_lines += XR_REGION_USABLE_LINES;
    }

    // Current block
    if (heap->current_block) {
        stats->total_blocks++;
        int live = xr_region_count_live_lines(heap->current_block);
        stats->live_lines += (size_t) live;
        stats->free_lines += (size_t) (XR_REGION_USABLE_LINES - live);
    }

#undef SCAN_LIST

    stats->total_bytes = stats->total_blocks * XR_REGION_BLOCK_SIZE;
}
