/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_gc.c - Per-Coroutine Immix Mark-Region GC
 */

#include "xcoro_gc.h"
#include "xgc_internal.h"
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
#include "xstackmap.h"     // XrStackMapTable, XrStackMapEntry
#include "xbc_stackmap.h"  // XrBcStackMap, bytecode precise GC scanning
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
    return (coro && coro->isolate) ? coro->isolate->sys_heap : NULL;
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

static inline bool xr_gc_needs_finalize_ext(XrCoroGC *gc, uint8_t type) {
    if (type >= XR_NATIVE_TYPE_MAX)
        return false;
    if (g_type_ops[type].destroy)
        return true;
    XrayIsolate *iso = gc_get_isolate(gc);
    return iso && (iso->ext_finalize_bitmap & (1ULL << type));
}

static inline XrGCDestroyFn get_destroy_func_ext(XrCoroGC *gc, uint8_t type) {
    if (type >= XGC_MAX_TYPES)
        return NULL;
    XrGCDestroyFn fn = g_type_ops[type].destroy;
    if (fn)
        return fn;
    XrayIsolate *iso = gc_get_isolate(gc);
    return iso ? iso->ext_destroy_funcs[type] : NULL;
}

/*
 * Reset GC runtime state fields to initial values.
 * Shared by xr_coro_gc_create and xr_coro_gc_reset.
 * Does NOT touch: immix heap, shared_refs buffers, tuning params
 * (gc_pause, gc_stepmul), or owner pointer.
 */
static void gc_init_runtime_state(XrCoroGC *gc) {
    gc->totalbytes = 0;
    gc->large_bytes = 0;
    gc->in_gc = 0;
    gc->gc_disabled = 0;
    gc->gc_count = 0;
    gc->object_count = 0;
    gc->gc_time_ns = 0;
    gc->last_gc_time_ns = 0;
    gc->finalizer_count = 0;
}

/* ========== Coroutine GC Lifecycle ========== */

XrCoroGC *xr_coro_gc_create(struct XrCoroutine *coro, const XrCoroGCConfig *config) {
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

    // Initialize Immix heap
    xr_immix_init(&gc->immix);

    gc_init_runtime_state(gc);

    gc->gc_pause = config && config->gc_pause > 0 ? config->gc_pause : XR_SPAWN_CORO_GC_PAUSE;
    gc->gc_stepmul =
        config && config->gc_stepmul > 0 ? config->gc_stepmul : XR_SPAWN_CORO_GC_STEPMUL;

    gc->owner = coro;

    return gc;
}

/* ========== Common Helpers for Destroy/Reset ========== */

static void gc_free_root_callbacks(XrCoroGC *gc) {
    XrCoroGCRootEntry *entry = gc->root_callbacks;
    while (entry) {
        XrCoroGCRootEntry *next = entry->next;
        xr_free(entry);
        entry = next;
    }
    gc->root_callbacks = NULL;
}

// Call finalizers on all Immix objects across all block lists
static void gc_finalize_immix_objects(XrCoroGC *gc) {
    XrImmixBlock *blists[] = {gc->immix.full_blocks, gc->immix.recycle_blocks,
                              gc->immix.current_block, gc->immix.old_blocks};
    for (int i = 0; i < 4; i++) {
        for (XrImmixBlock *b = blists[i]; b; b = b->next) {
            if (!b->has_finalizers)
                continue;
            for (XrGCHeader *obj = b->local_allgc; obj; obj = obj->gc_next) {
                if (obj->extra & XR_OBJ_DEAD)
                    continue; /* already finalized + freed by RC drop */
                if (xr_gc_needs_finalize_ext(gc, obj->type)) {
                    XrGCDestroyFn destroy = get_destroy_func_ext(gc, obj->type);
                    if (destroy)
                        destroy(obj, gc);
                }
            }
            if (i == 2)
                break;  // current_block is single, not a list
        }
    }
}

// Finalize and free all large objects
static void gc_free_large_objects(XrCoroGC *gc) {
    XrGCHeader *lo = gc->large_objects;
    while (lo) {
        XrGCHeader *next = lo->gc_next;
        if (xr_gc_needs_finalize_ext(gc, lo->type)) {
            XrGCDestroyFn destroy = get_destroy_func_ext(gc, lo->type);
            if (destroy)
                destroy(lo, gc);
        }
        gc->large_bytes -= lo->objsize;
        if (XR_GC_IS_MMAP(lo)) {
            xr_mem_unmap(lo, lo->objsize);
        } else {
            xr_free(lo);
        }
        lo = next;
    }
    gc->large_objects = NULL;
}

// Decref all shared objects tracked by this GC cycle
static void gc_decref_all_shared(XrCoroGC *gc) {
    for (int i = 0; i < gc->shared_refs_count; i++) {
        XrGCHeader *obj = gc->shared_refs[i];
        int new_refc = xr_shared_decref(obj);
        if (new_refc == 0)
            xr_shared_destroy(obj);
    }
}

/* ========== Lifecycle ========== */

void xr_coro_gc_destroy(XrCoroGC *gc) {
    if (!gc)
        return;
    XR_DCHECK(!gc->in_gc, "gc_destroy called during GC");

    gc_free_root_callbacks(gc);
    gc_finalize_immix_objects(gc);
    xr_immix_destroy(&gc->immix);
    gc_free_large_objects(gc);
    xr_coro_gc_rc_freelist_destroy(gc);

    gc_decref_all_shared(gc);
    if (gc->shared_refs)
        xr_free(gc->shared_refs);
    if (gc->prev_shared_refs)
        xr_free(gc->prev_shared_refs);

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

    gc_free_root_callbacks(gc);
    gc_finalize_immix_objects(gc);
    xr_immix_reset(&gc->immix);
    gc_free_large_objects(gc);
    xr_coro_gc_rc_freelist_destroy(gc);

    gc_decref_all_shared(gc);
    gc->shared_refs_count = 0;
    gc->prev_shared_refs_count = 0;

    // Reset runtime state (keep tuning params and shared_refs buffers)
    gc_init_runtime_state(gc);
    gc->owner = new_owner;
}

/* ========== Allocation Helpers ========== */

/*
 * Link Immix object to block's local_allgc list and mark allocation lines.
 * Shared by xr_coro_gc_newobj (interpreter) and xr_jit_alloc_post (JIT).
 */
static inline void gc_post_immix_alloc(XrCoroGC *gc, XrGCHeader *obj, uint8_t type,
                                       uint32_t total) {
    XrImmixBlock *block = XR_IMMIX_BLOCK_FROM_PTR(obj);
    obj->gc_next = block->local_allgc;
    block->local_allgc = obj;
    block->alloc_count++;
    block->alloc_bytes += (int64_t) total;
    if (xr_gc_needs_finalize_ext(gc, type))
        block->has_finalizers = 1;

    // Mark alloc_marks so allocator knows these lines are occupied.
    // NOTE: Do NOT advance mark_cursor here. JIT inline allocs bump
    // cursor without setting alloc_marks; advancing mark_cursor would
    // skip those unmarked lines, causing hole scanner to treat them as
    // free. flush_marks (called in slow path) will batch-mark from
    // mark_cursor to cursor, covering both JIT and interpreter allocs.
    xr_immix_mark_alloc_lines_fast(obj, total);
}

/*
 * Update allocation statistics after object creation.
 * Shared by xr_coro_gc_newobj and xr_jit_alloc_post. Tracing's debt-driven
 * collection trigger is gone (RC owns reclamation); only the byte and object
 * counters are kept for the gc.* introspection builtins.
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
    /* Large objects are individually malloc'd/mmap'd: free directly. */
    if (obj->objsize > XR_LARGE_OBJECT_THRESHOLD) {
        /* Unlink from large_objects list. */
        XrGCHeader **p = &gc->large_objects;
        while (*p && *p != obj)
            p = &(*p)->gc_next;
        if (*p == obj)
            *p = obj->gc_next;
        gc->large_bytes -= (int64_t) obj->objsize;
        gc->totalbytes -= (int64_t) obj->objsize;
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

    /* The freelist is linked through the object's first PAYLOAD word (the
     * word at header+sizeof(header)), NOT gc_next: gc_next must stay intact
     * so the block's local_allgc chain remains walkable at teardown. The
     * size class guarantees objsize >= sizeof(XrGCHeader) + sizeof(void*),
     * so this write stays inside the object's footprint. */
    void **link = (void **) ((char *) obj + sizeof(XrGCHeader));
    *link = gc->rc_freelist[cls];
    gc->rc_freelist[cls] = obj;
}

XR_FUNC void xr_coro_gc_rc_freelist_destroy(XrCoroGC *gc) {
    if (!gc || !gc->rc_freelist)
        return;
    /* The blocks themselves live in Immix and are released in bulk when the
     * coroutine's heap is torn down; only the index array is owned here. */
    xr_free(gc->rc_freelist);
    gc->rc_freelist = NULL;
}

/* drop-to-zero reclamation: run the type destructor (if any), then return
 * the block to the size-class freelist. Shared (cross-coroutine) objects
 * use the atomic shared-destroy path instead of the per-coroutine freelist.
 * Region objects never reach here (their drop is a no-op). */
/* drop-to-zero reclamation: run the type destructor (if any), then return
 * the block to the size-class freelist. Shared (cross-coroutine) objects
 * use the atomic shared-destroy path instead of the per-coroutine freelist.
 * Region objects never reach here (their drop is a no-op). */
XR_FUNC void xr_coro_gc_rc_destroy(XrCoroGC *gc, XrGCHeader *obj) {
    if (!obj)
        return;

    /* Shared objects: atomic refcount + shared destroy (not coro-local). */
    if (XR_GC_IS_SHARED(obj)) {
        xr_shared_destroy(obj);
        return;
    }

    /* Run the type destructor (closes files/sockets, frees side buffers). */
    if (gc && xr_gc_needs_finalize_ext(gc, obj->type)) {
        XrGCDestroyFn destroy = get_destroy_func_ext(gc, obj->type);
        if (destroy) {
            destroy(obj, gc);
            gc->finalizer_count++;
        }
    }

    /* Mark DEAD before reclamation so the coroutine-teardown finalize walk
     * skips it — its destructor has already run here. This must be set
     * regardless of whether the block lands on the freelist (small objects
     * below the free-link minimum are not freelisted but still must not be
     * finalized twice). Cleared by newobj (extra = 0) when the block is
     * reused. */
    obj->extra |= XR_OBJ_DEAD;

    /* Return memory to the freelist (or free large/mmap directly). */
    if (gc)
        xr_coro_gc_rc_free(gc, obj);
}

XrGCHeader *xr_coro_gc_newobj(XrCoroGC *gc, uint8_t type, size_t size) {
    if (!gc)
        return NULL;
    XR_DCHECK(type < XGC_MAX_TYPES, "invalid GC type");
    XR_DCHECK(size >= sizeof(XrGCHeader), "alloc size too small for GC header");
    XR_DCHECK(gc->owner != NULL, "GC has no owner coroutine");

    size_t total = XGC_ALIGN(size);
    XrGCHeader *obj;

    bool use_mmap = false;
    if (total > XR_LARGE_OBJECT_THRESHOLD) {
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
        obj->gc_next = gc->large_objects;
        gc->large_objects = obj;
        gc->large_bytes += (int64_t) total;
    } else {
        /* RC freelist fast path: reuse a same-size-class block freed by a
         * previous drop-to-zero before falling back to Immix bump. */
        int cls = xr_rc_size_class(total);
        if (cls >= 0 && gc->rc_freelist && gc->rc_freelist[cls]) {
            obj = gc->rc_freelist[cls];
            void **link = (void **) ((char *) obj + sizeof(XrGCHeader));
            gc->rc_freelist[cls] = (XrGCHeader *) *link;
            /* Reused block: still linked in its block's local_allgc (gc_next
             * intact) and its alloc line is still marked. type/refcount/extra
             * are reset below; XR_OBJ_DEAD is cleared by extra = 0. */
        } else {
            obj = (XrGCHeader *) xr_immix_alloc(&gc->immix, total);
            if (!obj)
                return NULL;
            gc_post_immix_alloc(gc, obj, type, (uint32_t) total);
        }
    }

    obj->type = type;
    obj->objsize = (uint32_t) total;
    obj->extra = 0;  // Always clear extra (Immix memory may be uninitialized)
    /* RC: a freshly allocated object has exactly one owning reference (its
     * definition site). dup/drop are 1-based, so initialize to 1. Immix
     * memory is reused/uninitialized, so this must be set explicitly. */
    obj->refcount = 1;
    if (use_mmap)
        XR_GC_SET_MMAP(obj);

    // byte 9 (formerly tracing `marked`) left zero — tracing retired; RC owns
    // reclamation. refcount is initialized to 1 by the caller (newobj).

    gc_update_alloc_stats(gc, (uint32_t) total);

    return obj;
}

/* ========== Mark (retired) ==========
 *
 * Tracing is retired; reference counting owns reclamation. markobject was
 * the tri-color mark entry; it is now a no-op kept only so the (dead)
 * traverse helpers still link during staged removal. Deleted with the
 * traverse subsystem. Shared-object lifetime is the atomic shared-RC
 * (xshared.h); the per-cycle shared_refs tracking that tracing used is gone. */

void xr_coro_gc_markobject(XrCoroGC *gc, XrGCHeader *obj) {
    (void) gc;
    (void) obj;
}

/* ========== GC Step / Full GC (retired) ==========
 *
 * Reference counting owns reclamation (drop-to-zero frees via the
 * per-coroutine RC freelist; coroutine teardown bulk-frees the rest). The
 * tracing collector is retired, so both entry points are no-ops. They remain
 * as the public API surface (gc.collect(), allocation step hook) until those
 * call sites are themselves removed. */

void xr_coro_gc_step(XrCoroGC *gc) {
    (void) gc;
}

void xr_coro_gc_fullgc(XrCoroGC *gc) {
    (void) gc;
}

/* ========== External Root Registration ========== */

int xr_coro_gc_register_root(XrCoroGC *gc, XrCoroGCRootCallback callback, void *userdata) {
    if (!gc || !callback)
        return -1;

    XrCoroGCRootEntry *entry = (XrCoroGCRootEntry *) xr_malloc(sizeof(XrCoroGCRootEntry));
    if (!entry)
        return -1;

    entry->callback = callback;
    entry->userdata = userdata;
    entry->next = gc->root_callbacks;
    gc->root_callbacks = entry;
    return 0;
}

int xr_coro_gc_unregister_root(XrCoroGC *gc, XrCoroGCRootCallback callback, void *userdata) {
    if (!gc || !callback)
        return -1;

    XrCoroGCRootEntry **pp = &gc->root_callbacks;
    while (*pp) {
        XrCoroGCRootEntry *entry = *pp;
        if (entry->callback == callback && entry->userdata == userdata) {
            *pp = entry->next;
            xr_free(entry);
            return 0;
        }
        pp = &entry->next;
    }
    return -1;
}

/* ========== Debug ========== */

void xr_coro_gc_print_stats(XrCoroGC *gc) {
    if (!gc) {
        printf("XrCoroGC: NULL\n");
        return;
    }

    printf("=== XrCoroGC (Immix bump + RC) ===\n");
    printf("Total bytes:  %lld\n", (long long) gc->totalbytes);
    printf("GC count:     %u\n", gc->gc_count);
    printf("GC pause:     %d%%\n", gc->gc_pause);
    printf("GC stepmul:   %d\n", gc->gc_stepmul);

    XrImmixStats istats;
    xr_immix_get_stats(&gc->immix, &istats);
    printf("Immix blocks:   %zu (full=%zu recycle=%zu free=%zu)\n", istats.total_blocks,
           istats.full_blocks, istats.recycle_blocks, istats.free_blocks);
    printf("Immix memory:   %zu bytes\n", istats.total_bytes);
    printf("Immix lines:    live=%zu free=%zu\n", istats.live_lines, istats.free_lines);

    size_t obj_count = 0;
    XrImmixBlock *plists[] = {gc->immix.full_blocks, gc->immix.recycle_blocks,
                              gc->immix.current_block};
    for (int i = 0; i < 3; i++) {
        for (XrImmixBlock *b = plists[i]; b; b = b->next) {
            for (XrGCHeader *o = b->local_allgc; o; o = o->gc_next)
                obj_count++;
        }
        if (i == 2)
            break;
    }
    printf("Object count:   %zu\n", obj_count);

    size_t large_count = 0;
    for (XrGCHeader *obj = gc->large_objects; obj; obj = obj->gc_next)
        large_count++;
    printf("Large objects:  %zu (%lld bytes)\n", large_count, (long long) gc->large_bytes);

    printf("Finalizers total: %u\n", gc->finalizer_count);

    printf("=====================================\n");
}

/* ========== JIT Allocation Helper ========== */

// Slow path: full allocation when inline bump fails
// CALL_C convention: (coro, packed_arg)
// packed_arg = (uint64_t)gc_type << 32 | aligned_size
XrGCHeader *xr_jit_alloc(struct XrCoroutine *coro, uint64_t type_and_size) {
    uint8_t type = (uint8_t) (type_and_size >> 32);
    uint32_t size = (uint32_t) (type_and_size & 0xFFFFFFFF);
    if (!coro || !coro->coro_gc)
        return NULL;
    XR_DCHECK(type < XGC_MAX_TYPES, "jit_alloc: invalid GC type");
    XR_DCHECK(size >= sizeof(XrGCHeader), "jit_alloc: size too small");
    // Flush deferred alloc_marks from JIT inline allocs before calling
    // xr_coro_gc_newobj, which advances mark_cursor = cursor after its
    // own allocation. Without this flush, lines occupied by prior JIT
    // inline allocs would never be marked, causing the hole scanner to
    // treat them as free and allocate overlapping objects.
    xr_immix_flush_marks(&coro->coro_gc->immix);
    return xr_coro_gc_newobj(coro->coro_gc, type, size);
}

// Lightweight alloc_marks setter for JIT inline alloc fast path.
// CALL_C convention: (coro, obj_ptr_as_uint64)
// Only sets line occupancy bits; local_allgc and stats handled inline by JIT.
void xr_jit_mark_lines(struct XrCoroutine *coro, uint64_t obj_ptr) {
    (void) coro;
    void *p = (void *) (uintptr_t) obj_ptr;
    if (!p)
        return;
    XrGCHeader *obj = (XrGCHeader *) p;
    xr_immix_mark_alloc_lines_fast(obj, obj->objsize);
}

// Fast path post-alloc: GC bookkeeping after inline bump succeeds
// GC header already initialized by JIT code. This handles:
//   1. alloc_marks (line occupancy bitmap)
//   2. allgc link (per-block object list for sweep)
//   3. stats update (totalbytes, object_count, GCdebt)
// CALL_C convention: (coro, obj_ptr)
void xr_jit_alloc_post(struct XrCoroutine *coro, void *obj_ptr) {
    if (!coro || !coro->coro_gc || !obj_ptr)
        return;
    XrCoroGC *gc = coro->coro_gc;
    XrGCHeader *obj = (XrGCHeader *) obj_ptr;
    uint32_t total = obj->objsize;
    XR_DCHECK(total > 0, "jit_alloc_post: zero objsize");
    XR_DCHECK(total <= XR_LARGE_OBJECT_THRESHOLD, "jit_alloc_post: oversized for Immix");

    gc_post_immix_alloc(gc, obj, obj->type, total);
    gc_update_alloc_stats(gc, total);
}
