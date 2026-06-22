/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_heap.c - Per-Coroutine Memory Manager (Region bump + RC reclamation)
 */

#include "xcoro_heap.h"
#include "xgc_internal.h"
#include "xweak_registry.h"
#include "../../coro/xcoroutine.h"
#include "../../coro/xworker.h"
#include "../value/xvalue.h"
#include "../value/xslot_type.h"
#include "../object/xmap.h"
#include "../object/xset.h"
#include "../object/xnative_type.h"  // XR_NATIVE_TYPE_MAX
#include "../../base/xchecks.h"
#include "../xshared.h"
#include "xsystem_heap.h"
#include "../object/xstring.h"
#include "../core/xr_runtime_core.h"
#include "../xisolate_api.h"
#include "../xisolate_internal.h"
#include "../../runtime/xexec_state.h"
#include "../../runtime/xglobal_dict.h"
#include "../value/xstruct_layout.h"
#include "../class/xclass.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../base/xmalloc.h"
#include "../../os/os_mem.h"
#include "../../os/os_thread.h"

/* ========== Coroutine Heap Struct Two-Level Pool ========== */
/*
 * L1: per-Worker heap_free_list (lock-free, max 32).
 * L2: per-isolate stack on XrSystemHeap (mutex protected).
 *
 * L1 miss → L2 → xr_malloc.
 * L1 full → L2 → xr_free if L2 full.
 * Worker exit flushes L1 → L2 (xr_coro_heap_flush_pool).
 *
 * The L2 pool was previously a process-wide static (g_gc_pool_*), which
 * crossed isolate boundaries and made teardown ordering observable
 * across unrelated isolates. It is now scoped to XrSystemHeap so each
 * isolate owns its recycle stack and xr_sysheap_destroy reclaims the
 * remaining structs.
 */
#define XR_CORO_HEAP_POOL_L1_MAX 32

// Resolve the system heap that owns the L2 pool for a given coroutine
// or coroutine heap. Returns NULL only when the bootstrap path has not
// yet wired the runtime core, in which case callers fall back to malloc/free.
static inline XrSystemHeap *coro_heap_pool_from_coro(struct XrCoroutine *coro) {
    return (coro && coro->core) ? coro->core->sys_heap : NULL;
}

static inline XrSystemHeap *coro_heap_pool_from_heap(XrCoroHeap *heap) {
    return (heap && heap->owner) ? coro_heap_pool_from_coro(heap->owner) : NULL;
}

/* ========== Helper Functions ========== */

// Per-type destroy capability lookups from the runtime core.

static inline XrayIsolate *coro_heap_isolate(XrCoroHeap *heap) {
    return (heap && heap->owner) ? heap->owner->isolate : NULL;
}

static inline XrRuntimeCore *coro_heap_core(XrCoroHeap *heap) {
    return (heap && heap->owner) ? heap->owner->core : NULL;
}

static inline bool coro_heap_value_to_header(XrValue value, XrObjHeader **out) {
    if (!XR_VALUE_NEEDS_GC(value))
        return false;
    XrObjHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj)
        return false;
    if (out)
        *out = obj;
    return true;
}

static inline bool coro_heap_header_is_heap_value(XrObjHeader *obj) {
    XrObjHeader *roundtrip = NULL;
    return obj && coro_heap_value_to_header(XR_FROM_PTR(obj), &roundtrip) && roundtrip == obj;
}

static inline bool coro_heap_type_needs_destroy(XrCoroHeap *heap, uint8_t type) {
    return xr_runtime_core_type_needs_destroy(coro_heap_core(heap), type);
}

static inline XrObjDestroyFn coro_heap_destroy_func(XrCoroHeap *heap, uint8_t type) {
    return xr_runtime_core_destroy_op(coro_heap_core(heap), type);
}

/*
 * Reset coroutine heap runtime state fields to initial values.
 * Shared by xr_coro_heap_create and xr_coro_heap_reset.
 * Does NOT touch: region heap or owner pointer.
 */
static void coro_heap_init_runtime_state(XrCoroHeap *heap) {
    heap->totalbytes = 0;
    heap->large_bytes = 0;
    heap->is_collecting = 0;
    heap->cycle_collection_disabled = 0;
    heap->cycle_collecting = 0;
    heap->cycle_collect_threshold = XR_CYCLE_COLLECT_THRESHOLD_INIT;
    heap->cycle_count = 0;
    heap->object_count = 0;
    heap->gc_time_ns = 0;
    heap->last_gc_time_ns = 0;
    heap->finalizer_count = 0;
}

/* ========== Coroutine Heap Lifecycle ========== */

XrCoroHeap *xr_coro_heap_create(struct XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "coro_heap_create: NULL coroutine");
    XrCoroHeap *heap = NULL;

    // Fast path: L1 per-Worker free list (no lock)
    XrWorker *w = xr_current_worker();
    if (w && w->p.heap_free_list) {
        heap = w->p.heap_free_list;
        w->p.heap_free_list = *(XrCoroHeap **) heap;
        w->p.heap_free_count--;
    } else {
        // L2 per-isolate pool (mutex). Bootstrap before sys_heap exists
        // is rare and falls straight through to malloc.
        XrSystemHeap *system_heap = coro_heap_pool_from_coro(coro);
        heap = system_heap ? xr_sysheap_coro_heap_pool_pop(system_heap) : NULL;
        if (!heap) {
            heap = (XrCoroHeap *) xr_malloc(sizeof(XrCoroHeap));
        }
    }
    if (!heap)
        return NULL;

    memset(heap, 0, sizeof(XrCoroHeap));

    // Initialize Region heap
    xr_region_init(&heap->region);
    // Wire the runtime-core L2 block cache (NULL during bootstrap → OS alloc).
    heap->region.sys_heap = coro_heap_pool_from_coro(coro);

    coro_heap_init_runtime_state(heap);

    heap->owner = coro;

    return heap;
}

/* ========== GC Pointer Set (open addressing, tombstones) ========== */

// Fibonacci hash on the pointer; objects are >= 8-byte aligned so the
// low bits carry no entropy.
static inline uint32_t gc_ptrset_hash(const XrObjHeader *obj, uint32_t cap) {
    uint64_t h = ((uint64_t) (uintptr_t) obj >> 4) * 0x9E3779B97F4A7C15ull;
    return (uint32_t) (h >> 32) & (cap - 1);
}

// Rehash into a table of `new_cap` slots (power of two). Drops tombstones.
static bool gc_ptrset_rehash(XrGCPtrSet *set, uint32_t new_cap) {
    XrObjHeader **slots = (XrObjHeader **) xr_calloc(new_cap, sizeof(XrObjHeader *));
    if (!slots)
        return false;
    for (uint32_t i = 0; i < set->cap; i++) {
        XrObjHeader *obj = set->slots[i];
        if (!xr_gc_ptrset_slot_live(obj))
            continue;
        uint32_t idx = gc_ptrset_hash(obj, new_cap);
        while (slots[idx])
            idx = (idx + 1) & (new_cap - 1);
        slots[idx] = obj;
    }
    if (set->slots)
        xr_free(set->slots);
    set->slots = slots;
    set->cap = new_cap;
    set->tombstones = 0;
    return true;
}

// Ensure capacity for one more insert. Called BEFORE the object is
// allocated so registration after a successful alloc cannot fail (OOM
// surfaces here, mirroring the old prepare-node contract).
static bool gc_ptrset_reserve(XrGCPtrSet *set, uint32_t extra) {
    uint32_t needed = set->count + set->tombstones + extra;
    if (set->cap == 0)
        return gc_ptrset_rehash(set, 16);
    if (needed * 4 < set->cap * 3)
        return true;
    uint32_t new_cap = set->cap;
    while ((set->count + extra) * 4 >= new_cap * 3)
        new_cap <<= 1;
    return gc_ptrset_rehash(set, new_cap);
}

// Insert without allocation. Caller must have reserved capacity.
static void gc_ptrset_insert(XrGCPtrSet *set, XrObjHeader *obj) {
    XR_DCHECK(set->cap > 0, "ptrset_insert: no capacity reserved");
    uint32_t idx = gc_ptrset_hash(obj, set->cap);
    while (xr_gc_ptrset_slot_live(set->slots[idx])) {
        XR_DCHECK(set->slots[idx] != obj, "ptrset_insert: duplicate");
        idx = (idx + 1) & (set->cap - 1);
    }
    if (set->slots[idx] == XR_GC_PTRSET_TOMBSTONE)
        set->tombstones--;
    set->slots[idx] = obj;
    set->count++;
}

static bool gc_ptrset_remove(XrGCPtrSet *set, XrObjHeader *obj) {
    if (set->cap == 0 || set->count == 0)
        return false;
    uint32_t idx = gc_ptrset_hash(obj, set->cap);
    while (set->slots[idx]) {
        if (set->slots[idx] == obj) {
            set->slots[idx] = XR_GC_PTRSET_TOMBSTONE;
            set->count--;
            set->tombstones++;
            return true;
        }
        idx = (idx + 1) & (set->cap - 1);
    }
    return false;
}

// Take ownership of the set contents, leaving it empty. Used by the
// teardown walks so cascading unregisters from destructors operate on
// the (now empty) live set instead of invalidating the iteration.
static XrGCPtrSet gc_ptrset_take(XrGCPtrSet *set) {
    XrGCPtrSet taken = *set;
    memset(set, 0, sizeof(*set));
    return taken;
}

static void gc_ptrset_destroy(XrGCPtrSet *set) {
    if (set->slots)
        xr_free(set->slots);
    memset(set, 0, sizeof(*set));
}

/* ========== Common Helpers for Destroy/Reset ========== */

static void coro_heap_finalize_registered_objects(XrCoroHeap *heap) {
    XrGCPtrSet set = gc_ptrset_take(&heap->finalize_set);
    for (uint32_t i = 0; i < set.cap; i++) {
        XrObjHeader *obj = set.slots[i];
        if (!xr_gc_ptrset_slot_live(obj) || (obj->extra & XR_OBJ_DEAD))
            continue;
        obj->extra |= XR_OBJ_DEAD;
        XrObjDestroyFn destroy = coro_heap_destroy_func(heap, obj->type);
        if (destroy) {
            destroy(obj, heap);
            heap->finalizer_count++;
        }
    }
    gc_ptrset_destroy(&set);
}

// Finalize and free all large objects
static void coro_heap_free_large_objects(XrCoroHeap *heap) {
    XrGCPtrSet set = gc_ptrset_take(&heap->large_set);
    for (uint32_t i = 0; i < set.cap; i++) {
        XrObjHeader *lo = set.slots[i];
        if (!xr_gc_ptrset_slot_live(lo))
            continue;
        heap->large_bytes -= lo->objsize;
        if (XR_OBJ_IS_MMAP(lo)) {
            xr_mem_unmap(lo, lo->objsize);
        } else {
            xr_free(lo);
        }
    }
    gc_ptrset_destroy(&set);
    heap->large_bytes = 0;
}

// Reserve registration capacity BEFORE allocating the object, so a
// successful allocation can always be registered (OOM-safe ordering).
static bool coro_heap_prepare_registration(XrCoroHeap *heap, uint8_t type, size_t total,
                                           bool *needs_finalize, bool *is_large) {
    *needs_finalize = coro_heap_type_needs_destroy(heap, type);
    *is_large = total > XR_LARGE_OBJECT_THRESHOLD;
    if (*needs_finalize && !gc_ptrset_reserve(&heap->finalize_set, 1))
        return false;
    if (*is_large && !gc_ptrset_reserve(&heap->large_set, 1))
        return false;
    return true;
}

static void coro_heap_register_object(XrCoroHeap *heap, XrObjHeader *obj, bool needs_finalize,
                                      bool is_large) {
    if (is_large) {
        gc_ptrset_insert(&heap->large_set, obj);
        heap->large_bytes += (int64_t) obj->objsize;
    }
    if (needs_finalize) {
        gc_ptrset_insert(&heap->finalize_set, obj);
    }
}

static void coro_heap_unregister_finalizer(XrCoroHeap *heap, XrObjHeader *obj) {
    if (heap)
        (void) gc_ptrset_remove(&heap->finalize_set, obj);
}

static void coro_heap_unregister_large_object(XrCoroHeap *heap, XrObjHeader *obj) {
    if (heap)
        (void) gc_ptrset_remove(&heap->large_set, obj);
}

/* ========== Lifecycle ========== */

void xr_coro_heap_destroy(XrCoroHeap *heap) {
    if (!heap)
        return;
    XR_DCHECK(!heap->is_collecting, "coro_heap_destroy called while collecting");

    coro_heap_finalize_registered_objects(heap);
    xr_region_destroy(&heap->region);
    coro_heap_free_large_objects(heap);
    xr_coro_heap_recycler_destroy(heap);
    xr_cycle_roots_destroy(heap);

    // Recycle: try L1 (per-Worker), then L2 (per-isolate), then free
    XrWorker *w = xr_current_worker();
    if (w && w->p.heap_free_count < XR_CORO_HEAP_POOL_L1_MAX) {
        *(XrCoroHeap **) heap = w->p.heap_free_list;
        w->p.heap_free_list = heap;
        w->p.heap_free_count++;
    } else {
        XrSystemHeap *system_heap = coro_heap_pool_from_heap(heap);
        if (!system_heap || !xr_sysheap_coro_heap_pool_push(system_heap, heap)) {
            xr_free(heap);
        }
    }
}

// Flush a per-worker coroutine heap struct free list (L1) to the per-isolate
// pool (L2). Structs that don't fit in L2 are freed immediately.
// `heap` is the L2 owner; passing NULL forces every struct to malloc/free.
void xr_coro_heap_flush_pool(XrSystemHeap *heap, XrCoroHeap **free_list, int *count) {
    XR_DCHECK(free_list != NULL, "flush_pool: NULL free_list");
    XR_DCHECK(count != NULL, "flush_pool: NULL count");
    while (*free_list) {
        XrCoroHeap *coro_heap = *free_list;
        *free_list = *(XrCoroHeap **) coro_heap;
        if (!heap || !xr_sysheap_coro_heap_pool_push(heap, coro_heap)) {
            xr_free(coro_heap);
        }
    }
    *count = 0;
}

/*
 * Reset coroutine heap state for coroutine pool reuse.
 * Releases all objects but keeps the XrCoroHeap struct and gray list buffers.
 * Much cheaper than destroy+create cycle.
 */
void xr_coro_heap_reset(XrCoroHeap *heap, struct XrCoroutine *new_owner) {
    if (!heap)
        return;
    XR_DCHECK(new_owner != NULL, "coro_heap_reset: NULL new_owner");
    XR_DCHECK(!heap->is_collecting, "coro_heap_reset called while collecting");

    coro_heap_finalize_registered_objects(heap);
    xr_region_reset(&heap->region);
    coro_heap_free_large_objects(heap);
    xr_coro_heap_recycler_destroy(heap);
    xr_cycle_roots_destroy(heap);

    coro_heap_init_runtime_state(heap);
    heap->owner = new_owner;
    // Re-wire the per-isolate L2 block cache (region reset cleared it).
    heap->region.sys_heap = new_owner->isolate ? xr_isolate_get_sys_heap(new_owner->isolate) : NULL;
}

/* ========== Allocation Helpers ========== */

static inline void coro_heap_post_region_alloc(XrCoroHeap *heap, XrObjHeader *obj, uint8_t type,
                                               uint32_t total) {
    (void) heap;
    (void) type;
    // Per-block accounting drives whole-block reclaim (alloc_count tally vs
    // freelist dead-slot tally). No line bitmap: under pure RC there is no
    // intra-block reclamation, so occupancy is not tracked per line.
    XrRegionBlock *block = XR_REGION_BLOCK_FROM_PTR(obj);
    block->alloc_count++;
    block->alloc_bytes += (int64_t) total;
}

/*
 * Update allocation statistics after object creation.
 * Called by xr_coro_heap_new_obj. Tracing's debt-driven collection trigger is gone
 * (RC owns reclamation); only the byte and object counters are kept for the
 * gc.* introspection builtins.
 */
static inline void coro_heap_update_alloc_stats(XrCoroHeap *heap, uint32_t total) {
    heap->totalbytes += (int64_t) total;
    heap->object_count++;
    XR_DCHECK(heap->totalbytes >= 0, "totalbytes underflow");
}

/* ========== Allocation ========== */

/* ========== RC Per-Object Freelist ========== */

XR_FUNC void xr_coro_heap_recycle_obj(XrCoroHeap *heap, XrObjHeader *obj) {
    if (!heap || !obj)
        return;

    /* A dead object (rc==0) no longer counts toward live bytes — mirror the
     * coro_heap_update_alloc_stats add done at allocation. Small blocks return to the
     * size-class freelist (a later same-class alloc re-adds via
     * coro_heap_update_alloc_stats); large/mmap blocks are returned to the OS. Without
     * this, totalbytes (gc.count/countb/info) accumulated every small-object
     * allocation instead of tracking the live set. */
    heap->totalbytes -= (int64_t) obj->objsize;

    /* Large objects are individually malloc'd/mmap'd: free directly. */
    if (obj->objsize > XR_LARGE_OBJECT_THRESHOLD) {
        coro_heap_unregister_large_object(heap, obj);
        heap->large_bytes -= (int64_t) obj->objsize;
        if (XR_OBJ_IS_MMAP(obj))
            xr_mem_unmap(obj, obj->objsize);
        else
            xr_free(obj);
        return;
    }

    int cls = xr_rc_size_class(obj->objsize);
    if (cls < 0)
        return; /* too small for the free link (header-only) or oversized:
                 * not freelisted — reclaimed in bulk at coroutine teardown.
                 * XR_OBJ_DEAD was already set by xr_coro_heap_destroy_obj so the
                 * teardown finalize walk will not re-run the destructor. */

    /* Lazily allocate the freelist array on first free. */
    if (!heap->rc_freelist) {
        heap->rc_freelist = (XrObjHeader **) xr_calloc(XR_RC_FREECLASSES, sizeof(XrObjHeader *));
        if (!heap->rc_freelist)
            return; /* OOM: drop the block on the floor (bulk-freed at coro end) */
    }

    /* The freelist is linked through the object's first PAYLOAD word. The
     * size class guarantees objsize >= sizeof(XrObjHeader) + sizeof(void*),
     * so this write stays inside the object's footprint. */
    void **link = (void **) ((char *) obj + sizeof(XrObjHeader));
    *link = heap->rc_freelist[cls];
    heap->rc_freelist[cls] = obj;
}

XR_FUNC void xr_coro_heap_recycler_destroy(XrCoroHeap *heap) {
    if (!heap || !heap->rc_freelist)
        return;
    /* The blocks themselves live in Region and are released in bulk when the
     * coroutine's heap is torn down; only the index array is owned here. */
    xr_free(heap->rc_freelist);
    heap->rc_freelist = NULL;
}

/* Marker stored in XrRegionBlock.reclaim_dead_count once a block is decided
 * fully dead, so the freelist-rebuild and list-rebuild passes can recognize
 * it after the per-block dead-slot tally has been consumed. */
#define XR_BLOCK_RECLAIM_MARK 0xFFFFFFFFu

XR_FUNC void xr_coro_heap_reclaim_empty_blocks(XrCoroHeap *heap) {
    if (!heap || !heap->rc_freelist)
        return; /* nothing has been RC-freed yet → no dead slots to reclaim */
    XrRegionHeap *h = &heap->region;

    /* Pass 1: zero the dead-slot tally on every reclaim candidate (the
     * bump-retired lists) plus the current block. Freelist entries can point
     * into the current block; it is never reclaimed, but its stale tally must
     * not be misread as the reclaim marker below. free_blocks are empty and
     * hold no freelist entries, so they need no reset (activate_block clears
     * a block's counters when it is reused). */
    for (XrRegionBlock *b = h->full_blocks; b; b = b->next)
        b->reclaim_dead_count = 0;
    if (h->current_block)
        h->current_block->reclaim_dead_count = 0;

    /* Pass 2: tally dead slots per block from the RC freelists. Every slot a
     * block bump-allocated is either live or on a freelist, so a block whose
     * tally equals alloc_count has no live object left. */
    for (int cls = 0; cls < XR_RC_FREECLASSES; cls++) {
        for (XrObjHeader *o = heap->rc_freelist[cls]; o;) {
            void **link = (void **) ((char *) o + sizeof(XrObjHeader));
            XrObjHeader *next = (XrObjHeader *) *link;
            XR_REGION_BLOCK_FROM_PTR(o)->reclaim_dead_count++;
            o = next;
        }
    }

    /* Pass 3a: mark fully-dead retired blocks. Objects too small to be
     * freelisted (cls < 0) never appear in the tally, so a block holding one
     * simply will not reach alloc_count and is conservatively kept. */
    for (XrRegionBlock *b = h->full_blocks; b; b = b->next) {
        if (b->alloc_count > 0 && b->reclaim_dead_count == b->alloc_count)
            b->reclaim_dead_count = XR_BLOCK_RECLAIM_MARK;
    }

    /* Pass 3b: rebuild the freelists, dropping entries that live in a block
     * about to be reclaimed (their backing memory becomes reusable, so the
     * stale link words must not survive). Done BEFORE any block is reused. */
    for (int cls = 0; cls < XR_RC_FREECLASSES; cls++) {
        XrObjHeader *kept = NULL;
        for (XrObjHeader *o = heap->rc_freelist[cls]; o;) {
            void **link = (void **) ((char *) o + sizeof(XrObjHeader));
            XrObjHeader *next = (XrObjHeader *) *link;
            if (XR_REGION_BLOCK_FROM_PTR(o)->reclaim_dead_count != XR_BLOCK_RECLAIM_MARK) {
                *link = kept;
                kept = o;
            }
            o = next;
        }
        heap->rc_freelist[cls] = kept;
    }

    /* Pass 3c: move marked blocks from the retired lists to free_blocks for
     * size-class-agnostic reuse. activate_block resets their counters. */
    XrRegionBlock *new_full = NULL;
    for (XrRegionBlock *b = h->full_blocks; b;) {
        XrRegionBlock *next = b->next;
        if (b->reclaim_dead_count == XR_BLOCK_RECLAIM_MARK) {
            b->next = h->free_blocks;
            h->free_blocks = b;
        } else {
            b->next = new_full;
            new_full = b;
        }
        b = next;
    }
    h->full_blocks = new_full;
}

/* Maximum recursive destroy depth before switching to deferred mode.
 * Prevents stack overflow on deep data structures (linked lists 10K+
 * nodes, deeply nested trees). When exceeded, child objects are pushed
 * onto heap->deferred_drops and drained iteratively by the top-level call. */
#define XR_DESTROY_DEPTH_LIMIT 64

/* Core destroy logic (shared by top-level and deferred-drain paths). */
static void rc_destroy_one(XrCoroHeap *heap, XrObjHeader *obj) {
    XR_DCHECK(obj != NULL, "rc_destroy_one: NULL obj");
    if (heap && (obj->extra & XR_OBJ_WEAKABLE))
        xr_weak_registry_target_dying(coro_heap_isolate(heap), obj, heap);
    obj->extra |= XR_OBJ_DEAD;

    /* Destroy is the single convergence point for every drop path (VM
     * OP_DROP, container/field release, and the cycle
     * collector), so unlink a cycle-tracked object from cycle_roots here
     * while its memory is still valid. A stale pointer left in cycle_roots
     * would be aliased by a later same-size-class freelist reuse, putting
     * the same live object in the roots array twice (double trial-decrement
     * → use-after-free). Cleared before the freelist push below. */
    if (heap && (obj->extra & XR_OBJ_CYCLE_CANDIDATE))
        xr_cycle_remove_root(heap, obj);

    /* Run the type destructor (closes files/sockets, frees side buffers,
     * drops child references — which may push more onto deferred_drops). */
    if (heap && coro_heap_type_needs_destroy(heap, obj->type)) {
        XrObjDestroyFn destroy = coro_heap_destroy_func(heap, obj->type);
        if (destroy) {
            destroy(obj, heap);
            heap->finalizer_count++;
        }
        coro_heap_unregister_finalizer(heap, obj);
    }

    if (heap && heap->object_count > 0)
        heap->object_count--;

    /* Return memory to the freelist (or free large/mmap directly). */
    if (heap)
        xr_coro_heap_recycle_obj(heap, obj);
}

/* Push an object onto the deferred-drop list for iterative draining.
 * Uses the first pointer-sized region past the header as a next-link
 * (safe because the object is already logically dead / RC == 0). */
static void deferred_push(XrCoroHeap *heap, XrObjHeader *obj) {
    /* Encode the linked list via a cast to void** at header+1. */
    void **link = (void **) (obj + 1);
    *link = heap->deferred_drops;
    heap->deferred_drops = obj;
}

static XrObjHeader *deferred_pop(XrCoroHeap *heap) {
    XrObjHeader *obj = heap->deferred_drops;
    if (!obj)
        return NULL;
    void **link = (void **) (obj + 1);
    heap->deferred_drops = (XrObjHeader *) *link;
    return obj;
}

/* drop-to-zero reclamation: run the type destructor (if any), then return
 * the block to the size-class freelist. Shared (cross-coroutine) objects
 * use the atomic shared-destroy path instead of the per-coroutine freelist.
 * Region objects never reach here (their drop is a no-op).
 *
 * Implements depth-bounded recursion: when destroy_depth exceeds the limit,
 * child objects are deferred and drained iteratively by the outermost call.
 * This prevents stack overflow on pathological inputs (Koka-inspired). */
XR_FUNC void xr_coro_heap_destroy_obj(XrCoroHeap *heap, XrObjHeader *obj) {
    if (!obj)
        return;
    if (!coro_heap_header_is_heap_value(obj))
        return;
    if (obj->extra & XR_OBJ_DEAD)
        return;

    /* Shared objects: atomic refcount + shared destroy (not coro-local). */
    if (XR_OBJ_IS_SHARED(obj)) {
        xr_shared_destroy_core(coro_heap_core(heap), obj);
        return;
    }

    /* Depth-limit guard: if we are already deep in a recursive destroy
     * chain, defer this object for iterative processing later. */
    if (heap && heap->destroy_depth >= XR_DESTROY_DEPTH_LIMIT) {
        deferred_push(heap, obj);
        return;
    }

    /* Track recursion depth. */
    if (heap)
        heap->destroy_depth++;

    rc_destroy_one(heap, obj);

    /* If this is the outermost destroy call, drain any deferred objects
     * that accumulated from deep recursion during child drops. */
    if (heap) {
        heap->destroy_depth--;
        if (heap->destroy_depth == 0) {
            while (heap->deferred_drops) {
                XrObjHeader *deferred = deferred_pop(heap);
                if (deferred && !(deferred->extra & XR_OBJ_DEAD))
                    rc_destroy_one(heap, deferred);
            }
        }
    }
}

XrObjHeader *xr_coro_heap_new_obj(XrCoroHeap *heap, uint8_t type, size_t size) {
    if (!heap)
        return NULL;
    XR_DCHECK(type < XR_OBJ_TYPE_MAX, "invalid object type");
    XR_DCHECK(size >= sizeof(XrObjHeader), "alloc size too small for object header");
    XR_DCHECK(heap->owner != NULL, "coroutine heap has no owner coroutine");

    size_t total = XGC_ALIGN(size);
    XrObjHeader *obj;
    bool needs_finalize = false;
    bool is_large = false;

    if (!coro_heap_prepare_registration(heap, type, total, &needs_finalize, &is_large))
        return NULL;

    bool use_mmap = false;
    if (is_large) {
        if (total >= XR_MMAP_THRESHOLD) {
            // Tier 2: very large — use anonymous mmap (xr_mem_map)
            // to avoid libc heap fragmentation.
            obj = (XrObjHeader *) xr_mem_map(total, XR_MEM_PROT_READ | XR_MEM_PROT_WRITE);
            if (!obj)
                return NULL;
            use_mmap = true;
        } else {
            // Tier 1: medium large — use xr_malloc
            obj = (XrObjHeader *) xr_malloc(total);
            if (!obj)
                return NULL;
        }
    } else {
        /* RC freelist fast path: reuse a same-size-class block freed by a
         * previous drop-to-zero before falling back to Region bump. */
        int cls = xr_rc_size_class(total);
        if (cls >= 0 && heap->rc_freelist && heap->rc_freelist[cls]) {
            obj = heap->rc_freelist[cls];
            void **link = (void **) ((char *) obj + sizeof(XrObjHeader));
            heap->rc_freelist[cls] = (XrObjHeader *) *link;
            /* Reused block: its allocation line is still occupied. type/refcount/extra
             * are reset below; XR_OBJ_DEAD is cleared by extra = 0. */
        } else {
            obj = (XrObjHeader *) xr_region_alloc(&heap->region, total);
            if (!obj)
                return NULL;
            coro_heap_post_region_alloc(heap, obj, type, (uint32_t) total);
        }
    }

    obj->type = type;
    obj->objsize = (uint32_t) total;
    obj->extra = 0;  // Always clear extra (Region memory may be uninitialized)
    /* Not in cycle_roots yet. MUST be the sentinel, never 0: a freelist-reused
     * block that still read _rsv==0 would be treated as "already at roots index
     * 0", so add_root would refuse it and remove_root would corrupt roots[0]. */
    obj->_rsv = XR_CYCLE_NOT_IN_ROOTS;
    /* RC: a freshly allocated object has exactly one owning reference (its
     * definition site). The count is 0-based and sign-tagged, so the unique
     * value is XR_RC_INIT (0). Region memory is reused/uninitialized, so this
     * must be set explicitly. Relaxed store: a fresh object is not yet shared
     * across threads, so no ordering is needed. */
    atomic_store_explicit(&obj->refcount, XR_RC_INIT, memory_order_relaxed);
    if (use_mmap)
        XR_OBJ_SET_MMAP(obj);
    if (needs_finalize)
        XR_OBJ_SET_FLAG(obj, XR_OBJ_HAS_DTOR);

    coro_heap_register_object(heap, obj, needs_finalize, is_large);

    coro_heap_update_alloc_stats(heap, (uint32_t) total);

    return obj;
}

/* ========== Debug ========== */

void xr_coro_heap_print_stats(XrCoroHeap *heap) {
    if (!heap) {
        printf("XrCoroHeap: NULL\n");
        return;
    }

    printf("=== XrCoroHeap (Region bump + RC) ===\n");
    printf("Total bytes:  %lld\n", (long long) heap->totalbytes);
    printf("Cycle count:     %u\n", heap->cycle_count);

    XrRegionStats istats;
    xr_region_get_stats(&heap->region, &istats);
    printf("Region blocks:   %zu (full=%zu free=%zu)\n", istats.total_blocks, istats.full_blocks,
           istats.free_blocks);
    printf("Region memory:   %zu bytes\n", istats.total_bytes);

    printf("Object count:   %u\n", heap->object_count);

    printf("Large objects:  %u (%lld bytes)\n", heap->large_set.count,
           (long long) heap->large_bytes);

    printf("Finalizers total: %u\n", heap->finalizer_count);

    printf("=====================================\n");
}
