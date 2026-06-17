/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregion.h - Region Mark-Region memory allocator
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

#ifndef XREGION_H
#define XREGION_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "../../base/xdefs.h"
#include "../../base/xchecks.h"

/* ========== Constants ========== */

#define XR_REGION_BLOCK_SIZE (16 * 1024)
#define XR_REGION_LINE_SIZE 128
#define XR_REGION_FIRST_LINE 1  // Line 0 reserved for block metadata; objects start at line 1

/* ========== Address-to-Block Mapping ========== */

// Get XrRegionBlock metadata directly from any object pointer (no indirection).
// Blocks are BLOCK_SIZE-aligned; metadata is embedded in Line 0.
#define XR_REGION_BLOCK_FROM_PTR(ptr)                                                              \
    ((XrRegionBlock *) ((uintptr_t) (ptr) & ~((uintptr_t) (XR_REGION_BLOCK_SIZE - 1))))

/* ========== Block Metadata ========== */

/*
 * Block metadata lives in Line 0 (first 128B) of the 16KB aligned block.
 * No separate malloc needed — block pointer IS the data pointer.
 */
struct XrGCHeader;
struct XrSystemHeap;

typedef struct XrRegionBlock {
    struct XrRegionBlock *next;   // 8B  @0
    uint32_t alloc_count;         // 4B  @8  - distinct slots bump-allocated in this block
    int64_t alloc_bytes;          // 8B  @16 - total allocated bytes in the block
    uint32_t reclaim_dead_count;  // 4B  @24 - transient: dead-slot tally during whole-block
                                  //      reclaim (see xr_coro_gc_reclaim_blocks).
    // Keep this layout within Line 0 (128B).
} XrRegionBlock;

_Static_assert(sizeof(XrRegionBlock) <= XR_REGION_LINE_SIZE, "XrRegionBlock must fit in Line 0");

/* ========== Per-Coroutine Region Heap ========== */

/*
 * Each block is on exactly ONE of these lists, or is the current_block.
 *
 *   full_blocks - retired blocks (current block once its bump space is used up)
 *   free_blocks - completely empty, reusable (refilled by whole-block reclaim)
 *
 * Under pure RC there is no line-level reclamation: dead small objects return
 * to the per-coroutine size-class freelist, and a block re-enters free_blocks
 * only when whole-block reclaim finds every slot dead (see
 * xr_coro_gc_reclaim_blocks). There is therefore no "partially free / recycle"
 * list to maintain.
 */
typedef struct XrRegionHeap {
    // Hot path: bump allocation
    char *cursor;                  // offset 0
    char *limit;                   // offset 8
    XrRegionBlock *current_block;  // offset 16

    // Cold path: block list management
    XrRegionBlock *full_blocks;
    XrRegionBlock *free_blocks;
    size_t total_blocks;
    size_t total_block_bytes;

    // Per-isolate L2 block cache owner (opaque). Set after init from the
    // owning coroutine's isolate; NULL during bootstrap (falls back to the
    // OS allocator). Block reuse is thus scoped to the isolate, not global.
    struct XrSystemHeap *sys_heap;
} XrRegionHeap;

/* ========== Lifecycle API ========== */

XR_FUNC void xr_region_init(XrRegionHeap *heap);
XR_FUNC void xr_region_destroy(XrRegionHeap *heap);

// Reset heap for reuse (bulk free all blocks, reinitialize state)
XR_FUNC void xr_region_reset(XrRegionHeap *heap);

/* ========== Allocation API ========== */

// Slow path for allocation (hole scanning, new block)
XR_FUNC void *xr_region_alloc_slow(XrRegionHeap *heap, size_t size);

// Bump-pointer allocate `size` bytes (8-byte aligned).
// Fast path inlined for cross-unit performance.
static inline void *xr_region_alloc(XrRegionHeap *heap, size_t size) {
    XR_DCHECK(size > 0 && size <= XR_REGION_BLOCK_SIZE, "region_alloc: invalid size");
    size = (size + 7) & ~(size_t) 7;
    char *result = heap->cursor;
    if (__builtin_expect(result != NULL, 1)) {
        char *new_cursor = result + size;
        if (new_cursor <= heap->limit) {
            heap->cursor = new_cursor;
            XR_DCHECK(heap->cursor <= heap->limit, "region_alloc: cursor > limit");
            return result;
        }
    }
    return xr_region_alloc_slow(heap, size);
}

/* ========== Block Cache API ========== */

// Flush a per-worker L1 block cache to the isolate's L2 pool (`sys_heap`).
// Called from worker destroy to avoid block leaks. Blocks that don't fit are
// returned to the OS. Pass sys_heap=NULL to force every block to the OS.
struct XrSystemHeap;
XR_FUNC void xr_region_flush_block_cache(struct XrSystemHeap *sys_heap, void *block_cache[],
                                         int *count);

// Return a raw aligned block (as cached in the L2 pool) to the OS. Used by the
// system heap when draining its block pool at isolate teardown.
XR_FUNC void xr_region_free_raw_block(void *block);

/* ========== Debug API ========== */

typedef struct XrRegionStats {
    size_t total_blocks;
    size_t free_blocks;
    size_t full_blocks;
    size_t total_bytes;
} XrRegionStats;

XR_FUNC void xr_region_get_stats(XrRegionHeap *heap, XrRegionStats *stats);

#endif  // XREGION_H
