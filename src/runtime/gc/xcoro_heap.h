/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_heap.h - Per-Coroutine Memory Manager (Region bump + RC reclamation)
 *
 * Reclamation model:
 *   - Compile-time RC (xi_arc) owns object lifetime; drop-to-zero frees
 *     through the per-coroutine RC freelist (same-size-class reuse).
 *   - Shared objects (cross-coroutine) use atomic refcounting (xshared.h).
 *   - Coroutine teardown bulk-frees all Region blocks and large objects.
 *
 * Allocation:
 *   - Small objects (≤4 KB): Region bump-pointer inside 16 KB blocks;
 *     objects never move, C extensions are naturally safe.
 *   - Large objects (>4 KB): individual xr_malloc / os_mmap.
 *
 * Tracing write barriers are retired: reference counting owns reclamation.
 */

#ifndef XCORO_HEAP_H
#define XCORO_HEAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "../../base/xdefs.h"
#include "../../base/xmalloc.h"
#include "../value/xvalue.h"
#include "xregion.h"
#include "xgc_internal.h"

/* ========== Forward Declarations ========== */

#ifndef XR_VALUE_DEFINED
typedef struct XrValue XrValue;
#endif
struct XrCoroutine;
struct XrFixedHeap;

/* ========== Large Object Threshold ========== */

#define XR_LARGE_OBJECT_THRESHOLD (4 * 1024)  // >4KB → large object (xr_malloc)
#define XR_MMAP_THRESHOLD (256 * 1024)        // ≥256KB → mmap (avoid libc heap fragmentation)

/* ========== RC Per-Object Freelist ========== */
/*
 * Small RC-managed objects (≤ XR_LARGE_OBJECT_THRESHOLD) are returned to a
 * per-coroutine segregated freelist when their refcount hits zero, then
 * reused by subsequent allocations of the same size class. This gives RC a
 * real single-object reclamation path on top of the Region bump allocator,
 * which itself never frees individual objects.
 *
 * Size classes use EXACT 8-byte granularity — the same alignment the
 * allocator rounds every object up to the heap allocation alignment. Exact classes are
 * mandatory: a freed slot's physical footprint is fixed at its first
 * allocation, and newobj overwrites objsize with the new request on reuse.
 * A coarser granularity would let a larger request pop a smaller freed slot
 * and overflow it into the adjacent object. With 8-byte classes every member
 * of a class has the identical aligned size, so reuse is byte-exact.
 *
 * Minimum freelisted size is sizeof(XrObjHeader) + one pointer: the free link
 * is stored in the object's first payload word (header+sizeof(header)), so an
 * object with no payload (header-only, 24 bytes) has nowhere to put it without
 * clobbering the adjacent object. Such objects are not freelisted — they are
 * reclaimed in bulk at coroutine teardown.
 */
#define XR_RC_FREE_GRANULARITY 8 /* must equal XR_HEAP_ALIGN_SIZE (allocator alignment) */
#define XR_RC_FREE_MIN_SIZE (sizeof(XrObjHeader) + sizeof(void *)) /* room for the free link */
#define XR_RC_FREECLASSES (XR_LARGE_OBJECT_THRESHOLD / XR_RC_FREE_GRANULARITY)  // 512

/* The freelist size-class step must match the allocator's alignment exactly:
 * every object in a class then has the identical aligned footprint, so a
 * reused slot fits the new request byte-for-byte. If these diverge, a larger
 * allocation could pop a smaller freed slot and overflow the neighbor. */
_Static_assert(XR_RC_FREE_GRANULARITY == XR_HEAP_ALIGN_SIZE,
               "RC freelist granularity must equal the heap allocator alignment");

/* Map an aligned allocation size to its freelist class index, or -1 if the
 * size is out of range: too small to hold the free link, or larger than the
 * large-object threshold (those are malloc/mmap-backed, freed individually). */
static inline int xr_rc_size_class(size_t aligned_size) {
    if (aligned_size < XR_RC_FREE_MIN_SIZE || aligned_size > XR_LARGE_OBJECT_THRESHOLD)
        return -1;
    return (int) (aligned_size / XR_RC_FREE_GRANULARITY) - 1;
}

/* ========== Heap Pointer Set (open-addressing hash set) ========== */
/*
 * Tracks per-coroutine object registrations that must support O(1)
 * insert/remove on the RC hot path:
 *   - finalize set: objects whose type has a destructor (teardown hook)
 *   - large set: malloc/mmap-backed objects freed individually
 *
 * Open addressing with tombstones; capacity is a power of two and grows
 * when (live + tombstones) exceeds 3/4 of capacity. Insert never fails
 * after a successful heap_ptrset_reserve (no allocation on insert), which
 * preserves the allocator's "reserve bookkeeping before object alloc"
 * OOM contract.
 */

typedef struct XrHeapPtrSet {
    XrObjHeader **slots;  // NULL slot = empty; XR_HEAP_PTRSET_TOMBSTONE = deleted
    uint32_t cap;         // power of two (0 until first reserve)
    uint32_t count;       // live entries
    uint32_t tombstones;  // deleted slots awaiting rehash
} XrHeapPtrSet;

#define XR_HEAP_PTRSET_TOMBSTONE ((XrObjHeader *) (uintptr_t) 1)

static inline bool xr_heap_ptrset_slot_live(XrObjHeader *slot) {
    return slot != NULL && slot != XR_HEAP_PTRSET_TOMBSTONE;
}

/* ========== Coroutine Heap Structure (Region bump + RC reclamation) ========== */

typedef struct XrCoroHeap {
    // === Cache line 0: Region allocator hot path ===
    // cursor/limit/current_block are the first 3 fields of XrRegionHeap.
    XrRegionHeap region;

    // === Allocation accounting ===
    int64_t totalbytes;                 // Total allocated bytes (gc.count / gc.info stats)
    uint8_t is_collecting;              // Re-entry guard (teardown / reset)
    uint8_t cycle_collection_disabled;  // gc.disable/enable counter: gates the automatic
                                        // cycle collector (xr_cycle_add_root auto-trigger)
    uint8_t cycle_collecting;           // Re-entry guard for the auto-triggered cycle collector
    uint8_t _pad1[5];                   // alignment

    // === Large objects (malloc/mmap-backed; freed individually at teardown) ===
    XrHeapPtrSet large_set;  // All large objects (O(1) insert/remove)
    int64_t large_bytes;     // Total bytes registered in large_set

    // Ownership
    struct XrCoroutine *owner;

    // Objects that need teardown finalization if they outlive local RC.
    // O(1) remove here is load-bearing: every drop-to-zero of an object
    // with a destructor unregisters it, so a linked list would make RC
    // reclamation O(live objects) and teardown O(n^2).
    XrHeapPtrSet finalize_set;

    // Statistics (cold; surfaced by memory/collection introspection builtins)
    uint32_t cycle_collect_count;  // Number of cycle collector runs
    uint32_t object_count;         // Live heap object count (incremental counter)
    uint64_t cycle_collect_time_ns;
    uint64_t last_cycle_collect_time_ns;
    uint32_t finalizer_count;  // Total finalizers called

    // === RC per-object freelist (RC reclaims small objects) ===
    // Segregated free lists by size class, lazily allocated; on drop-to-zero a
    // small object's memory is pushed here and reused by a later same-class
    // allocation before falling back to Region bump. NULL until the first free.
    XrObjHeader **rc_freelist;  // array[XR_RC_FREECLASSES] of list heads

    // === Stackless recursive free ===
    // Prevents stack overflow when destroying deep data structures (linked
    // lists with 10K+ nodes, deeply nested trees). When destroy_depth exceeds
    // the threshold, objects are pushed onto deferred_drops instead of being
    // destroyed recursively. The top-level destroy call drains the queue
    // iteratively before returning.
    uint16_t destroy_depth;       // current recursion depth of rc_destroy
    uint16_t _pad_drop[3];        // alignment
    XrObjHeader *deferred_drops;  // singly-linked list via GCHeader (reuse a field)

    // === Cycle collector (Bacon-Rajan trial deletion) ===
    // Potential cycle roots: objects whose type is XR_OBJ_CYCLE_CANDIDATE and
    // whose RC was decremented but did not reach zero. The collector runs on
    // gc.collect() and frees dead cycles that pure RC cannot reclaim.
    XrObjHeader **cycle_roots;         // growable array of potential roots (NULL until first add)
    uint32_t cycle_root_count;         // number of entries in cycle_roots
    uint32_t cycle_root_cap;           // capacity of cycle_roots array
    uint32_t cycle_collect_threshold;  // auto-trigger fullgc when root count reaches this
} XrCoroHeap;

/* ========== Coroutine Heap Lifecycle API ========== */

XR_FUNC XrCoroHeap *xr_coro_heap_create(struct XrCoroutine *coro);
XR_FUNC void xr_coro_heap_destroy(XrCoroHeap *heap);
XR_FUNC void xr_coro_heap_reset(XrCoroHeap *heap, struct XrCoroutine *new_owner);

// Flush per-worker coroutine heap struct free list (L1) to the isolate-owned
// L2 pool stored on XrSystemHeap. Called from worker destroy to avoid
// struct leaks. Pass `heap=NULL` to force every struct back to malloc.
struct XrSystemHeap;
XR_FUNC void xr_coro_heap_flush_pool(struct XrSystemHeap *heap, struct XrCoroHeap **free_list,
                                     int *count);

/* ========== Coroutine Heap Allocation API ========== */

/*
 * Core coroutine-local allocation function:
 * 1. Allocate from the Region bump allocator or large-object path.
 * 2. Register large/finalized objects for teardown bookkeeping.
 * 3. Update RC-era live byte/object counters.
 */
XR_FUNC XrObjHeader *xr_coro_heap_new_obj(XrCoroHeap *heap, uint8_t type, size_t size);

/* ========== RC Freelist API ========== */

/* Push a small object's memory onto the RC freelist for its size class.
 * Called by drop-to-zero AFTER the destructor has run. The object's
 * `objsize` must still be valid. No-op for large/region/atomic objects. */
XR_FUNC void xr_coro_heap_recycle_obj(XrCoroHeap *heap, XrObjHeader *obj);

/* drop-to-zero reclamation: run the type destructor (if any) then return
 * the block to the freelist. Routes shared objects to xr_shared_destroy. */
XR_FUNC void xr_coro_heap_destroy_obj(XrCoroHeap *heap, XrObjHeader *obj);

/* Release the freelist array itself (block memory is owned by Region and
 * freed in bulk at coroutine teardown). Called from heap destroy/reset. */
XR_FUNC void xr_coro_heap_recycler_destroy(XrCoroHeap *heap);

/* NOTE: Perceus-style drop-reuse (FBIP) is deliberately NOT implemented here.
 * The per-coroutine size-class freelist above already reclaims and reuses
 * same-size blocks correctly and automatically; a block-level drop-reuse would
 * only duplicate it. True FBIP (in-place data-buffer/field reuse, e.g. a unique
 * array map) requires consuming-receiver methods plus uniqueness/move proof and
 * is tracked as a dedicated future effort, not a half-built op pair. */

// Convenience macros
#define xr_coro_heap_new_typed(heap, type, Type)                                                   \
    ((Type *) ((XrObjHeader *) xr_coro_heap_new_obj((heap), (type), sizeof(Type)) + 1))

// Cycle collection: runs the Bacon-Rajan trial deletion collector on
// accumulated cycle_roots, then clears the roots list. Called by gc.collect().
XR_FUNC void xr_coro_heap_collect_cycles(XrCoroHeap *heap);

/* Whole-block reclaim: return fully-dead Region blocks to the heap's free pool
 * so a later allocation of ANY size class can reuse them. Bounds peak
 * retention of long-lived coroutines under shifting size-class loads, where a
 * per-size-class RC freelist alone never lets size B reuse size A's memory.
 * Not on the alloc/free hot path — called from xr_coro_heap_collect_cycles. */
XR_FUNC void xr_coro_heap_reclaim_empty_blocks(XrCoroHeap *heap);

/* === Cycle collector API === */

/* Add a cycle-candidate object to the cycle_roots set after its RC was
 * decremented but stayed > 0. Uses _rsv as root_idx for O(1) removal.
 * No-op if the object is already in the set or is not a cycle candidate. */
XR_FUNC void xr_cycle_add_root(XrCoroHeap *heap, XrObjHeader *obj);

/* Remove an object from cycle_roots (e.g. when it reaches RC==0 via normal
 * drop before a collect cycle runs). O(1) via swap-with-last. */
XR_FUNC void xr_cycle_remove_root(XrCoroHeap *heap, XrObjHeader *obj);

/* Free the cycle_roots array. Called at coroutine teardown. */
XR_FUNC void xr_cycle_roots_destroy(XrCoroHeap *heap);

/* Sentinel value for _rsv meaning "not in cycle_roots". */
#define XR_CYCLE_NOT_IN_ROOTS 0xFFFFFFFFu

/* Cycle-collector auto-trigger thresholds (root-count based, Nim ORC style).
 * The collector runs automatically once cycle_roots grows past the current
 * threshold; the threshold then adapts to the collection's productivity:
 * a productive collect lowers it (collect more eagerly), an unproductive one
 * raises it (avoid churn). Bounded below by _MIN so it never thrashes. */
#define XR_CYCLE_COLLECT_THRESHOLD_INIT 128u
#define XR_CYCLE_COLLECT_THRESHOLD_MIN 16u

/* ========== Unified Compile-Time RC Primitives ==========
 *
 * The single authoritative dup/drop entry points. EVERY reference-counting
 * site routes through these: the VM OP_DUP/OP_DROP dispatch and the
 * container/field element retain/release in the object runtime. Keeping one
 * implementation means the DEAD guard and the cycle-root bookkeeping cannot
 * drift apart between paths (the historical bug: OP_DROP bypassed cycle-root
 * tracking that only the container
 * path performed, so local-variable cycles leaked and stale freelist
 * entries could alias back into cycle_roots).
 *
 * Cycle-root contract (see xcycle_collector.c):
 *   - release that reaches RC==0 destroys the object; rc_destroy_one()
 *     unlinks it from cycle_roots, so no stale pointer survives into the
 *     freelist.
 *   - release that leaves RC>0 on a cycle-candidate registers the object
 *     as a potential cycle root for trial deletion.
 */

static inline void xr_rc_retain(XrObjHeader *o) {
    if (!o || (o->extra & (XR_OBJ_DEAD | XR_OBJ_STORAGE_BUMP)))
        return;
    xr_obj_dup(o);
}

static inline void xr_rc_release(XrCoroHeap *heap, XrObjHeader *o) {
    if (!o || (o->extra & (XR_OBJ_DEAD | XR_OBJ_STORAGE_BUMP)))
        return;
    if (xr_obj_drop_is_last(o)) {
        xr_coro_heap_destroy_obj(heap, o);
    } else if (o->extra & XR_OBJ_CYCLE_CANDIDATE) {
        xr_cycle_add_root(heap, o);
    }
}

static inline void xr_rc_retain_value(XrValue value) {
    if (!XR_IS_PTR(value))
        return;
    xr_rc_retain((XrObjHeader *) XR_VALUE_GCPTR(value));
}

static inline void xr_rc_release_value(XrCoroHeap *heap, XrValue value) {
    if (!XR_IS_PTR(value))
        return;
    xr_rc_release(heap, (XrObjHeader *) XR_VALUE_GCPTR(value));
}

/* ========== External Memory Accounting ========== */

/*
 * Track non-GC malloc'd memory (e.g., array/map/set data buffers) in the
 * coroutine's byte counter. Under reference counting this no longer drives
 * collection (RC owns reclamation); it keeps gc.count()/gc.info() byte stats
 * accurate so abandoned large buffers are reflected in reported usage.
 */
static inline void xr_coro_heap_add_external(XrCoroHeap *heap, int64_t bytes) {
    if (!heap)
        return;
    heap->totalbytes += bytes;
}

static inline void xr_coro_heap_sub_external(XrCoroHeap *heap, int64_t bytes) {
    if (!heap)
        return;
    heap->totalbytes -= bytes;
}

/* ========== Query API ========== */

static inline size_t xr_coro_heap_total_bytes(XrCoroHeap *heap) {
    return heap ? (size_t) heap->totalbytes : 0;
}

static inline bool xr_coro_heap_is_collecting(XrCoroHeap *heap) {
    return heap && heap->is_collecting;
}

/* ========== Debug API ========== */

XR_FUNC void xr_coro_heap_print_stats(XrCoroHeap *heap);

#endif  // XCORO_HEAP_H
