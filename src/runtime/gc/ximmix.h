/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ximmix.h - Immix Mark-Region memory allocator
 *
 * KEY CONCEPT:
 *   Block-Line architecture for fast bump-pointer allocation with
 *   line-granularity memory reclamation. Objects don't move.
 *
 * MEMORY LAYOUT:
 *   Block (16KB, 16KB-aligned):
 *   +---------------------+ Line 0 (128B) - metadata pointer + padding
 *   +---------------------+ Line 1
 *   | Object data...      |
 *   |                     | Lines 1-127 usable for objects (16256B)
 *   +---------------------+ Line 127
 *   | Object data...      |
 *   +---------------------+
 *
 * WHY THIS DESIGN:
 *   - Bump pointer allocation as fast as arena
 *   - Dead memory reclaimable at 128B line granularity
 *   - Empty blocks returned to free pool (or OS)
 *   - Spawn coroutines bulk-free all blocks at once
 */

#ifndef XIMMIX_H
#define XIMMIX_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "../../base/xdefs.h"
#include "../../base/xchecks.h"

/* ========== Constants ========== */

#define XR_IMMIX_BLOCK_SIZE     (16 * 1024)
#define XR_IMMIX_LINE_SIZE      128
#define XR_IMMIX_LINES          (XR_IMMIX_BLOCK_SIZE / XR_IMMIX_LINE_SIZE)  // 128
#define XR_IMMIX_FIRST_LINE     1   // Line 0 reserved for metadata pointer
#define XR_IMMIX_USABLE_LINES   (XR_IMMIX_LINES - XR_IMMIX_FIRST_LINE)     // 127

/* ========== Line Mark Bitmap ========== */

/*
 * 128 lines = 2 x uint64_t bitmap.
 * Single bitmap design:
 *   alloc_marks - the only bitmap, always reflects true line occupancy.
 *   SET on allocation, rebuilt during sweep (per-block).
 */
#define XR_IMMIX_LINE_SET(bm, line) \
    ((bm)[(line) >> 6] |= (1ULL << ((line) & 63)))

#define XR_IMMIX_LINE_GET(bm, line) \
    ((bm)[(line) >> 6] & (1ULL << ((line) & 63)))

/* ========== Address-to-Block/Line Mapping ========== */

// Block data is BLOCK_SIZE-aligned
#define XR_IMMIX_BLOCK_DATA(ptr) \
    ((char*)((uintptr_t)(ptr) & ~((uintptr_t)(XR_IMMIX_BLOCK_SIZE - 1))))

// Get XrImmixBlock metadata directly from any object pointer (no indirection)
// Metadata is embedded in Line 0 of the aligned block
#define XR_IMMIX_BLOCK_FROM_PTR(ptr) \
    ((XrImmixBlock*)((uintptr_t)(ptr) & ~((uintptr_t)(XR_IMMIX_BLOCK_SIZE - 1))))

#define XR_IMMIX_LINE_INDEX(ptr) \
    ((int)(((uintptr_t)(ptr) & (XR_IMMIX_BLOCK_SIZE - 1)) / XR_IMMIX_LINE_SIZE))

/* ========== Block Metadata ========== */

/*
 * Block metadata lives in Line 0 (first 128B) of the 16KB aligned block.
 * No separate malloc needed — block pointer IS the data pointer.
 */
struct XrGCHeader;

typedef struct XrImmixBlock {
    struct XrImmixBlock *next;      // 8B
    uint64_t alloc_marks[2];        // 16B - line occupancy (alloc + live)
    struct XrGCHeader *local_allgc; // 8B  - per-block object list (cache-friendly sweep)
    uint8_t next_scan_line;         // 1B  - resume position for hole scanning
    uint8_t is_young;               // 1B  - Sticky Immix: 1=young block, 0=old block
    uint8_t has_marked;             // 1B  - set during mark if any object in block is marked
    uint8_t has_finalizers;         // 1B  - set if any object with finalizer allocated here
    uint8_t has_black;              // 1B  - set when any object in block is set2black (GEN survivors)
    uint8_t _pad2[3];               // 3B  - padding
    uint32_t alloc_count;           // 4B  - number of objects in local_allgc
    int64_t alloc_bytes;            // 8B  - total bytes of objects in local_allgc
    // Total: 56B (with alignment padding), fits in Line 0 (128B)
} XrImmixBlock;

_Static_assert(sizeof(XrImmixBlock) <= XR_IMMIX_LINE_SIZE,
               "XrImmixBlock must fit in Line 0");

/* ========== Per-Coroutine Immix Heap ========== */

/*
 * Each block is on exactly ONE of these lists, or is the current_block.
 *
 *   full_blocks    - all usable lines occupied by live objects
 *   recycle_blocks - has free holes (rebuilt after each GC reclaim)
 *   free_blocks    - completely empty, reusable
 */
typedef struct XrImmixHeap {
    // Hot path: bump allocation (JIT inline candidates)
    char *cursor;                // offset 0 — JIT hardcoded
    char *limit;                 // offset 8 — JIT hardcoded
    XrImmixBlock *current_block; // offset 16

    // Deferred alloc_marks: JIT fast path bumps cursor without setting marks.
    // Before hole-scanning, flush marks from mark_cursor to cursor.
    char *mark_cursor;

    // Cold path: block list management (young blocks only in gen mode)
    XrImmixBlock *full_blocks;
    XrImmixBlock *recycle_blocks;
    XrImmixBlock *free_blocks;
    size_t total_blocks;
    size_t total_block_bytes;

    // Sticky Immix: old blocks (skipped by minor GC)
    XrImmixBlock *old_blocks;
    size_t old_block_count;
} XrImmixHeap;

/* ========== Lifecycle API ========== */

XR_FUNC void xr_immix_init(XrImmixHeap *heap);
XR_FUNC void xr_immix_destroy(XrImmixHeap *heap);

// Reset heap for reuse (bulk free all blocks, reinitialize state)
XR_FUNC void xr_immix_reset(XrImmixHeap *heap);

/* ========== Allocation API ========== */

// Slow path for allocation (hole scanning, new block)
XR_FUNC void* xr_immix_alloc_slow(XrImmixHeap *heap, size_t size);

// Bump-pointer allocate `size` bytes (8-byte aligned).
// Fast path inlined for cross-unit performance.
static inline void* xr_immix_alloc(XrImmixHeap *heap, size_t size) {
    XR_DCHECK(size > 0 && size <= XR_IMMIX_BLOCK_SIZE, "immix_alloc: invalid size");
    size = (size + 7) & ~(size_t)7;
    char *result = heap->cursor;
    if (__builtin_expect(result != NULL, 1)) {
        char *new_cursor = result + size;
        if (new_cursor <= heap->limit) {
            heap->cursor = new_cursor;
            XR_DCHECK(heap->cursor <= heap->limit, "immix_alloc: cursor > limit");
            return result;
        }
    }
    return xr_immix_alloc_slow(heap, size);
}

/* ========== Deferred Mark Flush ========== */

// Flush deferred alloc_marks: mark all lines from mark_cursor to cursor.
// Must be called before any hole-scanning (slow path entry).
static inline void xr_immix_flush_marks(XrImmixHeap *heap) {
    char *mc = heap->mark_cursor;
    char *c  = heap->cursor;
    if (mc >= c || !mc) return;
    XrImmixBlock *block = XR_IMMIX_BLOCK_FROM_PTR(mc);
    int first = XR_IMMIX_LINE_INDEX(mc);
    int last  = XR_IMMIX_LINE_INDEX(c - 1);
    // Bulk set bits [first..last] using bitmask operations
    uint64_t lo_mask = ~((1ULL << (first & 63)) - 1);   // bits [first%64 .. 63]
    uint64_t hi_bit  = (last & 63) == 63 ? UINT64_MAX : (1ULL << ((last & 63) + 1)) - 1;
    if ((first >> 6) == (last >> 6)) {
        block->alloc_marks[first >> 6] |= lo_mask & hi_bit;
    } else {
        block->alloc_marks[0] |= lo_mask;
        block->alloc_marks[1] |= hi_bit;
    }
    heap->mark_cursor = c;
}

/* ========== GC Integration API (Single Bitmap) ========== */

// Mark alloc_marks for a newly allocated object.
XR_FUNC void xr_immix_mark_alloc_lines(void *obj_ptr, size_t obj_size);

/*
 * Inline fast path for marking alloc_marks for all lines an object spans.
 * An object smaller than a line can still cross a line boundary
 * (e.g., 40B object at offset 120 of a 128B line extends into the next line).
 */
static inline void xr_immix_mark_alloc_lines_fast(void *obj_ptr, size_t obj_size) {
    XrImmixBlock *block = XR_IMMIX_BLOCK_FROM_PTR(obj_ptr);
    int first = XR_IMMIX_LINE_INDEX(obj_ptr);
    int last  = XR_IMMIX_LINE_INDEX((char*)obj_ptr + obj_size - 1);
    XR_IMMIX_LINE_SET(block->alloc_marks, first);
    for (int l = first + 1; l <= last; l++)
        XR_IMMIX_LINE_SET(block->alloc_marks, l);
}

// Count live lines in a block (excluding reserved line 0)
XR_FUNC int xr_immix_count_live_lines(XrImmixBlock *block);

// Post-sweep: classify blocks into free / recycle / full.
XR_FUNC void xr_immix_reclaim(XrImmixHeap *heap);

// Sticky Immix: reclaim only young blocks after minor GC
XR_FUNC void xr_immix_reclaim_young(XrImmixHeap *heap);

// Sticky Immix: check if an object pointer is in a young block
static inline bool xr_immix_is_young_ptr(void *ptr) {
    XrImmixBlock *block = XR_IMMIX_BLOCK_FROM_PTR(ptr);
    return block->is_young;
}

/* ========== Block Cache API ========== */

// Flush per-worker L1 block cache to global L2.
// Called from worker destroy to avoid block leaks.
XR_FUNC void xr_immix_flush_block_cache(void *block_cache[], int *count);

/* ========== Debug API ========== */

typedef struct XrImmixStats {
    size_t total_blocks;
    size_t free_blocks;
    size_t recycle_blocks;
    size_t full_blocks;
    size_t total_bytes;
    size_t live_lines;
    size_t free_lines;
} XrImmixStats;

XR_FUNC void xr_immix_get_stats(XrImmixHeap *heap, XrImmixStats *stats);

#endif // XIMMIX_H
