/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_gc.c - Per-Coroutine Memory Manager (Region bump + RC reclamation)
 */

#include "xcoro_gc.h"
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

/* ========== GC Struct Two-Level Pool ========== */
/*
 * L1: per-Worker gc_free_list (lock-free, max 32).
 * L2: per-isolate stack on XrSystemHeap (mutex protected).
 *
 * L1 miss → L2 → xr_malloc.
 * L1 full → L2 → xr_free if L2 full.
 * Worker exit flushes L1 → L2 (xr_coro_gc_flush_pool).
 *
 * The L2 pool was previously a process-wide static (g_gc_pool_*), which
 * crossed isolate boundaries and made teardown ordering observable
 * across unrelated isolates. It is now scoped to XrSystemHeap so each
 * isolate owns its recycle stack and xr_sysheap_destroy reclaims the
 * remaining structs.
 */
#define XR_GC_POOL_L1_MAX 32

// Resolve the system heap that owns the L2 pool for a given coroutine
// or coroutine GC. Returns NULL only when the bootstrap path has not
// yet wired the isolate, in which case callers fall back to malloc/free.
static inline XrSystemHeap *gc_pool_heap_from_coro(struct XrCoroutine *coro) {
    return (coro && coro->isolate) ? xr_isolate_get_sys_heap(coro->isolate) : NULL;
}

static inline XrSystemHeap *gc_pool_heap_from_gc(XrCoroGC *gc) {
    return (gc && gc->owner) ? gc_pool_heap_from_coro(gc->owner) : NULL;
}

/* ========== Helper Functions ========== */

// Per-type GC capability lookups derived from g_type_ops.
//
// Compile-time types: ops table slot is NULL when the type is a leaf
// (no traverse) or resource-less (no destroy). Extension types
// registered via xr_register_extension_destroy / _traverse fall through
// to the per-isolate tables on XrayIsolate.

static inline XrayIsolate *gc_get_isolate(XrCoroGC *gc) {
    return (gc && gc->owner) ? gc->owner->isolate : NULL;
}

static inline bool coro_gc_value_to_header(XrValue value, XrGCHeader **out) {
    if (!XR_VALUE_NEEDS_GC(value))
        return false;
    XrGCHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj)
        return false;
    if (out)
        *out = obj;
    return true;
}

static inline bool coro_gc_header_is_heap_value(XrGCHeader *obj) {
    XrGCHeader *roundtrip = NULL;
    return obj && coro_gc_value_to_header(XR_FROM_PTR(obj), &roundtrip) && roundtrip == obj;
}

static inline bool xr_gc_needs_finalize_ext(XrCoroGC *gc, uint8_t type) {
    if (type >= XR_NATIVE_TYPE_MAX)
        return false;
    if (g_type_ops[type].destroy)
        return true;
    XrayIsolate *iso = gc_get_isolate(gc);
    return iso && (xr_isolate_get_ext_finalize_bitmap(iso) & (1ULL << type));
}

static inline XrGCDestroyFn get_destroy_func_ext(XrCoroGC *gc, uint8_t type) {
    if (type >= XGC_MAX_TYPES)
        return NULL;
    XrGCDestroyFn fn = g_type_ops[type].destroy;
    if (fn)
        return fn;
    XrayIsolate *iso = gc_get_isolate(gc);
    return iso ? (XrGCDestroyFn) xr_isolate_get_ext_destroy(iso, type) : NULL;
}

/*
 * Reset GC runtime state fields to initial values.
 * Shared by xr_coro_gc_create and xr_coro_gc_reset.
 * Does NOT touch: region heap or owner pointer.
 */
static void gc_init_runtime_state(XrCoroGC *gc) {
    gc->totalbytes = 0;
    gc->large_bytes = 0;
    gc->in_gc = 0;
    gc->gc_disabled = 0;
    gc->cycle_collecting = 0;
    gc->cycle_collect_threshold = XR_CYCLE_COLLECT_THRESHOLD_INIT;
    gc->gc_count = 0;
    gc->object_count = 0;
    gc->gc_time_ns = 0;
    gc->last_gc_time_ns = 0;
    gc->finalizer_count = 0;
}

/* ========== Coroutine GC Lifecycle ========== */

XrCoroGC *xr_coro_gc_create(struct XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "gc_create: NULL coroutine");
    XrCoroGC *gc = NULL;

    // Fast path: L1 per-Worker free list (no lock)
    XrWorker *w = xr_current_worker();
    if (w && w->p.gc_free_list) {
        gc = w->p.gc_free_list;
        w->p.gc_free_list = *(XrCoroGC **) gc;
        w->p.gc_free_count--;
    } else {
        // L2 per-isolate pool (mutex). Bootstrap before sys_heap exists
        // is rare and falls straight through to malloc.
        XrSystemHeap *heap = gc_pool_heap_from_coro(coro);
        gc = heap ? xr_sysheap_gc_pool_pop(heap) : NULL;
        if (!gc) {
            gc = (XrCoroGC *) xr_malloc(sizeof(XrCoroGC));
        }
    }
    if (!gc)
        return NULL;

    memset(gc, 0, sizeof(XrCoroGC));

    // Initialize Region heap
    xr_region_init(&gc->region);
    // Wire the per-isolate L2 block cache (NULL during bootstrap → OS alloc).
    gc->region.sys_heap = coro->isolate ? xr_isolate_get_sys_heap(coro->isolate) : NULL;

    gc_init_runtime_state(gc);

    gc->owner = coro;

    return gc;
}

/* ========== GC Pointer Set (open addressing, tombstones) ========== */

// Fibonacci hash on the pointer; objects are >= 8-byte aligned so the
// low bits carry no entropy.
static inline uint32_t gc_ptrset_hash(const XrGCHeader *obj, uint32_t cap) {
    uint64_t h = ((uint64_t) (uintptr_t) obj >> 4) * 0x9E3779B97F4A7C15ull;
    return (uint32_t) (h >> 32) & (cap - 1);
}

// Rehash into a table of `new_cap` slots (power of two). Drops tombstones.
static bool gc_ptrset_rehash(XrGCPtrSet *set, uint32_t new_cap) {
    XrGCHeader **slots = (XrGCHeader **) xr_calloc(new_cap, sizeof(XrGCHeader *));
    if (!slots)
        return false;
    for (uint32_t i = 0; i < set->cap; i++) {
        XrGCHeader *obj = set->slots[i];
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
static void gc_ptrset_insert(XrGCPtrSet *set, XrGCHeader *obj) {
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

static bool gc_ptrset_remove(XrGCPtrSet *set, XrGCHeader *obj) {
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

static void gc_finalize_registered_objects(XrCoroGC *gc) {
    XrGCPtrSet set = gc_ptrset_take(&gc->finalize_set);
    for (uint32_t i = 0; i < set.cap; i++) {
        XrGCHeader *obj = set.slots[i];
        if (!xr_gc_ptrset_slot_live(obj) || (obj->extra & XR_OBJ_DEAD))
            continue;
        obj->extra |= XR_OBJ_DEAD;
        XrGCDestroyFn destroy = get_destroy_func_ext(gc, obj->type);
        if (destroy) {
            destroy(obj, gc);
            gc->finalizer_count++;
        }
    }
    gc_ptrset_destroy(&set);
}

// Finalize and free all large objects
static void gc_free_large_objects(XrCoroGC *gc) {
    XrGCPtrSet set = gc_ptrset_take(&gc->large_set);
    for (uint32_t i = 0; i < set.cap; i++) {
        XrGCHeader *lo = set.slots[i];
        if (!xr_gc_ptrset_slot_live(lo))
            continue;
        gc->large_bytes -= lo->objsize;
        if (XR_GC_IS_MMAP(lo)) {
            xr_mem_unmap(lo, lo->objsize);
        } else {
            xr_free(lo);
        }
    }
    gc_ptrset_destroy(&set);
    gc->large_bytes = 0;
}

// Reserve registration capacity BEFORE allocating the object, so a
// successful allocation can always be registered (OOM-safe ordering).
static bool gc_prepare_registration(XrCoroGC *gc, uint8_t type, size_t total, bool *needs_finalize,
                                    bool *is_large) {
    *needs_finalize = xr_gc_needs_finalize_ext(gc, type);
    *is_large = total > XR_LARGE_OBJECT_THRESHOLD;
    if (*needs_finalize && !gc_ptrset_reserve(&gc->finalize_set, 1))
        return false;
    if (*is_large && !gc_ptrset_reserve(&gc->large_set, 1))
        return false;
    return true;
}

static void gc_register_object(XrCoroGC *gc, XrGCHeader *obj, bool needs_finalize, bool is_large) {
    if (is_large) {
        gc_ptrset_insert(&gc->large_set, obj);
        gc->large_bytes += (int64_t) obj->objsize;
    }
    if (needs_finalize) {
        gc_ptrset_insert(&gc->finalize_set, obj);
    }
}

static bool gc_register_finalizer_after_inline_alloc(XrCoroGC *gc, XrGCHeader *obj) {
    if (!xr_gc_needs_finalize_ext(gc, obj->type))
        return true;
    if (!gc_ptrset_reserve(&gc->finalize_set, 1))
        return false;
    gc_ptrset_insert(&gc->finalize_set, obj);
    XR_OBJ_SET_FLAG(obj, XR_OBJ_HAS_DTOR);
    return true;
}

static void gc_unregister_finalizer(XrCoroGC *gc, XrGCHeader *obj) {
    if (gc)
        (void) gc_ptrset_remove(&gc->finalize_set, obj);
}

static void gc_unregister_large_object(XrCoroGC *gc, XrGCHeader *obj) {
    if (gc)
        (void) gc_ptrset_remove(&gc->large_set, obj);
}

/* ========== Lifecycle ========== */

void xr_coro_gc_destroy(XrCoroGC *gc) {
    if (!gc)
        return;
    XR_DCHECK(!gc->in_gc, "gc_destroy called during GC");

    gc_finalize_registered_objects(gc);
    xr_region_destroy(&gc->region);
    gc_free_large_objects(gc);
    xr_coro_gc_rc_freelist_destroy(gc);
    xr_cycle_roots_destroy(gc);

    // Recycle: try L1 (per-Worker), then L2 (per-isolate), then free
    XrWorker *w = xr_current_worker();
    if (w && w->p.gc_free_count < XR_GC_POOL_L1_MAX) {
        *(XrCoroGC **) gc = w->p.gc_free_list;
        w->p.gc_free_list = gc;
        w->p.gc_free_count++;
    } else {
        XrSystemHeap *heap = gc_pool_heap_from_gc(gc);
        if (!heap || !xr_sysheap_gc_pool_push(heap, gc)) {
            xr_free(gc);
        }
    }
}

// Flush a per-worker GC struct free list (L1) to the per-isolate
// pool (L2). Structs that don't fit in L2 are freed immediately.
// `heap` is the L2 owner; passing NULL forces every struct to malloc/free.
void xr_coro_gc_flush_pool(XrSystemHeap *heap, XrCoroGC **free_list, int *count) {
    XR_DCHECK(free_list != NULL, "flush_pool: NULL free_list");
    XR_DCHECK(count != NULL, "flush_pool: NULL count");
    while (*free_list) {
        XrCoroGC *gc = *free_list;
        *free_list = *(XrCoroGC **) gc;
        if (!heap || !xr_sysheap_gc_pool_push(heap, gc)) {
            xr_free(gc);
        }
    }
    *count = 0;
}

/*
 * Reset GC state for coroutine pool reuse.
 * Releases all objects but keeps the XrCoroGC struct and gray list buffers.
 * Much cheaper than destroy+create cycle.
 */
void xr_coro_gc_reset(XrCoroGC *gc, struct XrCoroutine *new_owner) {
    if (!gc)
        return;
    XR_DCHECK(new_owner != NULL, "gc_reset: NULL new_owner");
    XR_DCHECK(!gc->in_gc, "gc_reset called during GC");

    gc_finalize_registered_objects(gc);
    xr_region_reset(&gc->region);
    gc_free_large_objects(gc);
    xr_coro_gc_rc_freelist_destroy(gc);
    xr_cycle_roots_destroy(gc);

    gc_init_runtime_state(gc);
    gc->owner = new_owner;
    // Re-wire the per-isolate L2 block cache (region reset cleared it).
    gc->region.sys_heap = new_owner->isolate ? xr_isolate_get_sys_heap(new_owner->isolate) : NULL;
}

/* ========== Allocation Helpers ========== */

static inline void gc_post_region_alloc(XrCoroGC *gc, XrGCHeader *obj, uint8_t type,
                                        uint32_t total) {
    (void) gc;
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
 * Called by xr_coro_gc_newobj. Tracing's debt-driven collection trigger is gone
 * (RC owns reclamation); only the byte and object counters are kept for the
 * gc.* introspection builtins.
 */
static inline void gc_update_alloc_stats(XrCoroGC *gc, uint32_t total) {
    gc->totalbytes += (int64_t) total;
    gc->object_count++;
    XR_DCHECK(gc->totalbytes >= 0, "totalbytes underflow");
}

/* ========== Allocation ========== */

/* ========== RC Per-Object Freelist ========== */

XR_FUNC void xr_coro_gc_rc_free(XrCoroGC *gc, XrGCHeader *obj) {
    if (!gc || !obj)
        return;

    /* A dead object (rc==0) no longer counts toward live bytes — mirror the
     * gc_update_alloc_stats add done at allocation. Small blocks return to the
     * size-class freelist (a later same-class alloc re-adds via
     * gc_update_alloc_stats); large/mmap blocks are returned to the OS. Without
     * this, totalbytes (gc.count/countb/info) accumulated every small-object
     * allocation instead of tracking the live set. */
    gc->totalbytes -= (int64_t) obj->objsize;

    /* Large objects are individually malloc'd/mmap'd: free directly. */
    if (obj->objsize > XR_LARGE_OBJECT_THRESHOLD) {
        gc_unregister_large_object(gc, obj);
        gc->large_bytes -= (int64_t) obj->objsize;
        if (XR_GC_IS_MMAP(obj))
            xr_mem_unmap(obj, obj->objsize);
        else
            xr_free(obj);
        return;
    }

    int cls = xr_rc_size_class(obj->objsize);
    if (cls < 0)
        return; /* too small for the free link (header-only) or oversized:
                 * not freelisted — reclaimed in bulk at coroutine teardown.
                 * XR_OBJ_DEAD was already set by xr_coro_gc_rc_destroy so the
                 * teardown finalize walk will not re-run the destructor. */

    /* Lazily allocate the freelist array on first free. */
    if (!gc->rc_freelist) {
        gc->rc_freelist = (XrGCHeader **) xr_calloc(XR_RC_FREECLASSES, sizeof(XrGCHeader *));
        if (!gc->rc_freelist)
            return; /* OOM: drop the block on the floor (bulk-freed at coro end) */
    }

    /* The freelist is linked through the object's first PAYLOAD word. The
     * size class guarantees objsize >= sizeof(XrGCHeader) + sizeof(void*),
     * so this write stays inside the object's footprint. */
    void **link = (void **) ((char *) obj + sizeof(XrGCHeader));
    *link = gc->rc_freelist[cls];
    gc->rc_freelist[cls] = obj;
}

XR_FUNC void xr_coro_gc_rc_freelist_destroy(XrCoroGC *gc) {
    if (!gc || !gc->rc_freelist)
        return;
    /* The blocks themselves live in Region and are released in bulk when the
     * coroutine's heap is torn down; only the index array is owned here. */
    xr_free(gc->rc_freelist);
    gc->rc_freelist = NULL;
}

/* Marker stored in XrRegionBlock.reclaim_dead_count once a block is decided
 * fully dead, so the freelist-rebuild and list-rebuild passes can recognize
 * it after the per-block dead-slot tally has been consumed. */
#define XR_BLOCK_RECLAIM_MARK 0xFFFFFFFFu

XR_FUNC void xr_coro_gc_reclaim_blocks(XrCoroGC *gc) {
    if (!gc || !gc->rc_freelist)
        return; /* nothing has been RC-freed yet → no dead slots to reclaim */
    XrRegionHeap *h = &gc->region;

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
        for (XrGCHeader *o = gc->rc_freelist[cls]; o;) {
            void **link = (void **) ((char *) o + sizeof(XrGCHeader));
            XrGCHeader *next = (XrGCHeader *) *link;
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
        XrGCHeader *kept = NULL;
        for (XrGCHeader *o = gc->rc_freelist[cls]; o;) {
            void **link = (void **) ((char *) o + sizeof(XrGCHeader));
            XrGCHeader *next = (XrGCHeader *) *link;
            if (XR_REGION_BLOCK_FROM_PTR(o)->reclaim_dead_count != XR_BLOCK_RECLAIM_MARK) {
                *link = kept;
                kept = o;
            }
            o = next;
        }
        gc->rc_freelist[cls] = kept;
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
 * onto gc->deferred_drops and drained iteratively by the top-level call. */
#define XR_DESTROY_DEPTH_LIMIT 64

/* Core destroy logic (shared by top-level and deferred-drain paths). */
static void rc_destroy_one(XrCoroGC *gc, XrGCHeader *obj) {
    XR_DCHECK(obj != NULL, "rc_destroy_one: NULL obj");
    if (gc && (obj->extra & XR_OBJ_WEAKABLE))
        xr_weak_registry_target_dying(gc_get_isolate(gc), obj, gc);
    obj->extra |= XR_OBJ_DEAD;

    /* Destroy is the single convergence point for every drop path (VM
     * OP_DROP, container/field release, and the cycle
     * collector), so unlink a cycle-tracked object from cycle_roots here
     * while its memory is still valid. A stale pointer left in cycle_roots
     * would be aliased by a later same-size-class freelist reuse, putting
     * the same live object in the roots array twice (double trial-decrement
     * → use-after-free). Cleared before the freelist push below. */
    if (gc && (obj->extra & XR_OBJ_CYCLE_CANDIDATE))
        xr_cycle_remove_root(gc, obj);

    /* Run the type destructor (closes files/sockets, frees side buffers,
     * drops child references — which may push more onto deferred_drops). */
    if (gc && xr_gc_needs_finalize_ext(gc, obj->type)) {
        XrGCDestroyFn destroy = get_destroy_func_ext(gc, obj->type);
        if (destroy) {
            destroy(obj, gc);
            gc->finalizer_count++;
        }
        gc_unregister_finalizer(gc, obj);
    }

    if (gc && gc->object_count > 0)
        gc->object_count--;

    /* Return memory to the freelist (or free large/mmap directly). */
    if (gc)
        xr_coro_gc_rc_free(gc, obj);
}

/* Push an object onto the deferred-drop list for iterative draining.
 * Uses the first pointer-sized region past the header as a next-link
 * (safe because the object is already logically dead / RC == 0). */
static void deferred_push(XrCoroGC *gc, XrGCHeader *obj) {
    /* Encode the linked list via a cast to void** at header+1. */
    void **link = (void **) (obj + 1);
    *link = gc->deferred_drops;
    gc->deferred_drops = obj;
}

static XrGCHeader *deferred_pop(XrCoroGC *gc) {
    XrGCHeader *obj = gc->deferred_drops;
    if (!obj)
        return NULL;
    void **link = (void **) (obj + 1);
    gc->deferred_drops = (XrGCHeader *) *link;
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
XR_FUNC void xr_coro_gc_rc_destroy(XrCoroGC *gc, XrGCHeader *obj) {
    if (!obj)
        return;
    if (!coro_gc_header_is_heap_value(obj))
        return;
    if (obj->extra & XR_OBJ_DEAD)
        return;

    /* Shared objects: atomic refcount + shared destroy (not coro-local). */
    if (XR_GC_IS_SHARED(obj)) {
        xr_shared_destroy(obj);
        return;
    }

    /* Depth-limit guard: if we are already deep in a recursive destroy
     * chain, defer this object for iterative processing later. */
    if (gc && gc->destroy_depth >= XR_DESTROY_DEPTH_LIMIT) {
        deferred_push(gc, obj);
        return;
    }

    /* Track recursion depth. */
    if (gc)
        gc->destroy_depth++;

    rc_destroy_one(gc, obj);

    /* If this is the outermost destroy call, drain any deferred objects
     * that accumulated from deep recursion during child drops. */
    if (gc) {
        gc->destroy_depth--;
        if (gc->destroy_depth == 0) {
            while (gc->deferred_drops) {
                XrGCHeader *deferred = deferred_pop(gc);
                if (deferred && !(deferred->extra & XR_OBJ_DEAD))
                    rc_destroy_one(gc, deferred);
            }
        }
    }
}

XrGCHeader *xr_coro_gc_newobj(XrCoroGC *gc, uint8_t type, size_t size) {
    if (!gc)
        return NULL;
    XR_DCHECK(type < XGC_MAX_TYPES, "invalid GC type");
    XR_DCHECK(size >= sizeof(XrGCHeader), "alloc size too small for GC header");
    XR_DCHECK(gc->owner != NULL, "GC has no owner coroutine");

    size_t total = XGC_ALIGN(size);
    XrGCHeader *obj;
    bool needs_finalize = false;
    bool is_large = false;

    if (!gc_prepare_registration(gc, type, total, &needs_finalize, &is_large))
        return NULL;

    bool use_mmap = false;
    if (is_large) {
        if (total >= XR_MMAP_THRESHOLD) {
            // Tier 2: very large — use anonymous mmap (xr_mem_map)
            // to avoid libc heap fragmentation.
            obj = (XrGCHeader *) xr_mem_map(total, XR_MEM_PROT_READ | XR_MEM_PROT_WRITE);
            if (!obj)
                return NULL;
            use_mmap = true;
        } else {
            // Tier 1: medium large — use xr_malloc
            obj = (XrGCHeader *) xr_malloc(total);
            if (!obj)
                return NULL;
        }
    } else {
        /* RC freelist fast path: reuse a same-size-class block freed by a
         * previous drop-to-zero before falling back to Region bump. */
        int cls = xr_rc_size_class(total);
        if (cls >= 0 && gc->rc_freelist && gc->rc_freelist[cls]) {
            obj = gc->rc_freelist[cls];
            void **link = (void **) ((char *) obj + sizeof(XrGCHeader));
            gc->rc_freelist[cls] = (XrGCHeader *) *link;
            /* Reused block: its allocation line is still occupied. type/refcount/extra
             * are reset below; XR_OBJ_DEAD is cleared by extra = 0. */
        } else {
            obj = (XrGCHeader *) xr_region_alloc(&gc->region, total);
            if (!obj)
                return NULL;
            gc_post_region_alloc(gc, obj, type, (uint32_t) total);
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
        XR_GC_SET_MMAP(obj);
    if (needs_finalize)
        XR_OBJ_SET_FLAG(obj, XR_OBJ_HAS_DTOR);

    gc_register_object(gc, obj, needs_finalize, is_large);

    gc_update_alloc_stats(gc, (uint32_t) total);

    return obj;
}

/* ========== Debug ========== */

void xr_coro_gc_print_stats(XrCoroGC *gc) {
    if (!gc) {
        printf("XrCoroGC: NULL\n");
        return;
    }

    printf("=== XrCoroGC (Region bump + RC) ===\n");
    printf("Total bytes:  %lld\n", (long long) gc->totalbytes);
    printf("GC count:     %u\n", gc->gc_count);

    XrRegionStats istats;
    xr_region_get_stats(&gc->region, &istats);
    printf("Region blocks:   %zu (full=%zu free=%zu)\n", istats.total_blocks, istats.full_blocks,
           istats.free_blocks);
    printf("Region memory:   %zu bytes\n", istats.total_bytes);

    printf("Object count:   %u\n", gc->object_count);

    printf("Large objects:  %u (%lld bytes)\n", gc->large_set.count, (long long) gc->large_bytes);

    printf("Finalizers total: %u\n", gc->finalizer_count);

    printf("=====================================\n");
}
