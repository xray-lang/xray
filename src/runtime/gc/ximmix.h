/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ximmix.h - Per-coroutine block-bump arena
 *
 * A coroutine-private allocator: bump-pointer allocation inside 16KB
 * blocks, with bulk free of every block at coroutine teardown. Objects
 * never move, so C extensions and interior pointers stay valid.
 *
 * RECLAMATION SPLIT:
 *   - Individual objects are reclaimed by reference counting through the
 *     per-coroutine RC freelist (xcoro_gc.c). This arena does NOT free
 *     individual objects.
 *   - Whole blocks are freed in bulk when the coroutine ends (or is
 *     reset for pool reuse). A spawn coroutine therefore tears its heap
 *     down in O(blocks), not O(objects).
 *
 * This was formerly an Immix mark-region collector. Tracing/sweep is
 * retired (RC owns object lifetime), so the line-mark bitmap, hole
 * reclamation and generational machinery are gone: the arena now only
 * hands out memory and owns block lifetime.
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

#define XR_IMMIX_BLOCK_SIZE (16 * 1024)
/* The block metadata (XrImmixBlock) lives at the start of the aligned
 * block; objects start after this reserved header region. Keeping it a
 * round 128 bytes leaves the object payload comfortably aligned. */
#define XR_IMMIX_HEADER_SIZE 128
#define XR_IMMIX_USABLE_SIZE (XR_IMMIX_BLOCK_SIZE - XR_IMMIX_HEADER_SIZE)

/* Recover the owning block (16KB-aligned) from any object pointer. */
#define XR_IMMIX_BLOCK_FROM_PTR(ptr)                                                               \
    ((XrImmixBlock *) ((uintptr_t) (ptr) & ~((uintptr_t) (XR_IMMIX_BLOCK_SIZE - 1))))

/* ========== Block Metadata ========== */

/*
 * Block metadata lives in the reserved header region of the 16KB aligned
 * block. The block pointer IS the data pointer (no separate malloc). The
 * only field needed now is the intrusive list link used by the heap's
 * full / free block lists and the cross-worker block cache.
 */
typedef struct XrImmixBlock {
    struct XrImmixBlock *next;  // intrusive link (full_blocks / free_blocks / cache)
} XrImmixBlock;

_Static_assert(sizeof(XrImmixBlock) <= XR_IMMIX_HEADER_SIZE, "XrImmixBlock must fit in the header");

/* ========== Per-Coroutine Block-Bump Heap ========== */

/*
 * Allocation bumps cursor toward limit inside current_block. When the
 * block is exhausted it is retired to full_blocks and a fresh block is
 * taken from free_blocks (or allocated). Nothing is reclaimed mid-life;
 * RC reuses freed object slots via the segregated freelist in xcoro_gc.c,
 * and teardown bulk-frees every block.
 */
typedef struct XrImmixHeap {
    // Hot path: bump allocation (JIT inline reads cursor/limit at fixed offsets)
    char *cursor;                 // offset 0 — JIT hardcoded
    char *limit;                  // offset 8 — JIT hardcoded
    XrImmixBlock *current_block;  // offset 16

    // Cold path: block list management
    XrImmixBlock *full_blocks;  // retired blocks, freed in bulk at teardown
    XrImmixBlock *free_blocks;  // empty blocks ready for reuse
    size_t total_blocks;
    size_t total_block_bytes;
} XrImmixHeap;

/* ========== Lifecycle API ========== */

XR_FUNC void xr_immix_init(XrImmixHeap *heap);
XR_FUNC void xr_immix_destroy(XrImmixHeap *heap);

// Reset heap for reuse (bulk free all blocks, reinitialize state)
XR_FUNC void xr_immix_reset(XrImmixHeap *heap);

/* ========== Allocation API ========== */

// Slow path: retire the current block and take/allocate a fresh one.
XR_FUNC void *xr_immix_alloc_slow(XrImmixHeap *heap, size_t size);

// Bump-pointer allocate `size` bytes (8-byte aligned).
// Fast path inlined for cross-unit performance.
static inline void *xr_immix_alloc(XrImmixHeap *heap, size_t size) {
    XR_DCHECK(size > 0 && size <= XR_IMMIX_USABLE_SIZE, "immix_alloc: invalid size");
    size = (size + 7) & ~(size_t) 7;
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

/* ========== Block Cache API ========== */

// Flush per-worker L1 block cache to global L2.
// Called from worker destroy to avoid block leaks.
XR_FUNC void xr_immix_flush_block_cache(void *block_cache[], int *count);

/* ========== Debug API ========== */

typedef struct XrImmixStats {
    size_t total_blocks;
    size_t free_blocks;
    size_t full_blocks;
    size_t total_bytes;
} XrImmixStats;

XR_FUNC void xr_immix_get_stats(XrImmixHeap *heap, XrImmixStats *stats);

#endif  // XIMMIX_H
