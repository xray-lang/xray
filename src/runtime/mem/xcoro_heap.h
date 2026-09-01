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
 * Tracing barrier hooks are retired: reference counting owns reclamation.
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
#include "xobj_ops.h"
#include "xruntime_object_heap.h"

/* ========== Forward Declarations ========== */

#ifndef XR_VALUE_DEFINED
typedef struct XrValue XrValue;
#endif
struct XrCoroutine;
struct XrFixedHeap;
struct XrRuntimeObjectAllocation;

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
    int64_t totalbytes;     // Total allocated bytes (runtime.liveBytes / runtime.info stats)
    uint8_t is_collecting;  // Re-entry guard (teardown / reset)
                            // auto-trigger)
    // Set for the whole destroy/reset sequence. While it is set, drop-to-zero
    // reclaims nothing individually: the finalize walk iterates a snapshot of
    // finalize_set and reads each entry's header to skip already-destroyed
    // objects, so a destructor cascade that freed a snapshot entry outright
    // (large objects go straight back to malloc/munmap) would dangle it. Every
    // block is reclaimed in bulk right after the walk instead.
    uint8_t is_tearing_down;
    uint8_t _pad1[4];  // alignment

    // === Large objects (malloc/mmap-backed; freed individually at teardown) ===
    XrHeapPtrSet large_set;  // All large objects (O(1) insert/remove)
    int64_t large_bytes;     // Total bytes registered in large_set

    // Ownership
    /* The runtime core this heap belongs to. NOT a coroutine: a heap is a
     * region bump allocator plus an RC freelist plus three registries, and
     * none of that needs task identity. Only two things were ever read off
     * the old owner pointer — core, and core->sys_heap — so the heap now
     * names the core directly and a heap can exist without a coroutine.
     * That is what lets the VM's root execution own one. */
    struct XrRuntimeCore *core;

    // Objects that need teardown finalization if they outlive local RC.
    // O(1) remove here is load-bearing: every drop-to-zero of an object
    // with a destructor unregisters it, so a linked list would make RC
    // reclamation O(live objects) and teardown O(n^2).
    XrHeapPtrSet finalize_set;

    /* weak-field storage: target -> shared weak handle. Coroutine-local, so no
     * locking; only objects flagged XR_OBJ_HAS_WEAK ever look here. Declared
     * as an opaque struct so this header does not depend on xweak_handle.h. */
    struct XrWeakTable {
        struct XrWeakHandle **slots;
        uint32_t cap;
        uint32_t count;
        uint32_t tombstones;
    } weak_table;

    // Statistics (cold; surfaced by memory/collection introspection builtins)
    uint32_t object_count;     // Live heap object count (incremental counter)
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
    //
    // The queue is a side stack, not a list threaded through the objects: a
    // deferred object has only reached rc==0, its destructor has NOT run yet,
    // so every payload word it holds is still live (XrArray::data,
    // XrObjectInstance::klass, ...) and cannot be borrowed as a link field.
    uint16_t destroy_depth;        // current recursion depth of rc_destroy
    uint16_t _pad_drop[3];         // alignment
    XrObjHeader **deferred_drops;  // LIFO stack of objects awaiting destroy
    uint32_t deferred_drop_count;  // entries in use
    uint32_t deferred_drop_cap;    // allocated entries (NULL/0 until first defer)

    /* Canonical header-first runtime objects use allocation-prefix metadata
     * instead of storing allocator size/type facts in their ABI header. */
    struct XrRuntimeObjectAllocation *runtime_object_allocations;
} XrCoroHeap;

/* ========== Coroutine Heap Lifecycle API ========== */

XR_FUNC XrCoroHeap *xr_coro_heap_create(struct XrRuntimeCore *core);

/* In-place lifecycle, for a heap the caller owns (e.g. embedded in
 * XrRuntimeCore). These never touch the struct pool. */
XR_FUNC void xr_coro_heap_init_inplace(XrCoroHeap *heap, struct XrRuntimeCore *core);
XR_FUNC void xr_coro_heap_teardown_inplace(XrCoroHeap *heap);
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

/* Whole-block reclaim: return fully-dead Region blocks to the heap's free pool
 * so a later allocation of ANY size class can reuse them. Bounds peak
 * retention of long-lived coroutines under shifting size-class loads, where a
 * per-size-class RC freelist alone never lets size B reuse size A's memory.
 *
 * Not on the alloc/free hot path. Its only trigger used to be the removed cycle
 * collector; it currently has no caller and needs a size-class-pressure or
 * coroutine-yield trigger. The reclaim itself is orthogonal to cycles and
 * remains useful. */
XR_FUNC void xr_coro_heap_reclaim_empty_blocks(XrCoroHeap *heap);

/* ========== Unified Compile-Time RC Primitives ==========
 *
 * The single authoritative dup/drop entry points. EVERY reference-counting
 * site routes through these: the VM OP_DUP/OP_DROP dispatch and the
 * container/field element retain/release in the object runtime. Keeping one
 * implementation means the DEAD guard cannot drift apart between paths.
 *
 * Reclamation has exactly one rule: the object dies when its last strong
 * reference goes. A release that leaves RC > 0 does nothing further — there is
 * no collector to notify, no candidate set to join. Reference cycles are not
 * reclaimed at runtime; they are prevented statically (L0), broken explicitly
 * with `weak` (L1), and bounded by the coroutine heap (L2).
 */

static inline void xr_rc_retain(XrObjHeader *o) {
    if (!o || (o->extra & (XR_OBJ_DEAD | XR_OBJ_IMMORTAL)))
        return;
    xr_obj_dup(o);
}

static inline void xr_rc_release(XrCoroHeap *heap, XrObjHeader *o) {
    if (!o || (o->extra & (XR_OBJ_DEAD | XR_OBJ_IMMORTAL)))
        return;
    if (xr_obj_drop_is_last(o))
        xr_coro_heap_destroy_obj(heap, o);
}

static inline void xr_rc_retain_value(XrValue value) {
    if (!XR_IS_PTR(value))
        return;
    if (XR_HEAP_TYPE(value) == XR_TSTRING) {
        XrRuntimeAbiStatus status = xr_runtime_object_header_retain(
            (XrRuntimeObjectHeader *) XR_VALUE_GCPTR(value));
        XR_CHECK(status == XR_RUNTIME_ABI_OK,
                 "canonical string retain rejected invalid header state");
        return;
    }
    xr_rc_retain((XrObjHeader *) XR_VALUE_GCPTR(value));
}

static inline void xr_rc_release_value(XrCoroHeap *heap, XrValue value) {
    if (!XR_IS_PTR(value))
        return;
    if (XR_HEAP_TYPE(value) == XR_TSTRING) {
        (void) heap;
        XrRuntimeObjectHeader *header =
            (XrRuntimeObjectHeader *) XR_VALUE_GCPTR(value);
        bool last = false;
        XrRuntimeAbiStatus status =
            xr_runtime_object_header_release(header, &last);
        XR_CHECK(status == XR_RUNTIME_ABI_OK,
                 "canonical string release rejected invalid header state");
        if (last) {
            status = xr_runtime_object_reclaim(header);
            XR_CHECK(status == XR_RUNTIME_ABI_OK,
                     "canonical string allocation metadata is invalid");
        }
        return;
    }
    xr_rc_release(heap, (XrObjHeader *) XR_VALUE_GCPTR(value));
}

/* ========== External Memory Accounting ========== */

/*
 * Track non-GC malloc'd memory (e.g., array/map/set data buffers) in the
 * coroutine's byte counter. Under reference counting this no longer drives
 * collection (RC owns reclamation); it keeps runtime.liveBytes()/runtime.info() byte stats
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
