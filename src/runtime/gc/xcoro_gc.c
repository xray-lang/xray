/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_gc.c - Per-Coroutine Memory Manager (Immix bump + RC reclamation)
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
 * Does NOT touch: immix heap, tuning params (gc_pause, gc_stepmul),
 * or owner pointer.
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

static XrGCObjectNode *gc_object_node_new(XrGCHeader *obj) {
    XrGCObjectNode *node = (XrGCObjectNode *) xr_malloc(sizeof(XrGCObjectNode));
    if (!node)
        return NULL;
    node->obj = obj;
    node->next = NULL;
    return node;
}

static void gc_object_node_push(XrGCObjectNode **head, XrGCObjectNode *node, XrGCHeader *obj) {
    XR_DCHECK(head != NULL, "node_push: NULL head");
    XR_DCHECK(node != NULL, "node_push: NULL node");
    node->obj = obj;
    node->next = *head;
    *head = node;
}

static bool gc_object_node_remove(XrGCObjectNode **head, XrGCHeader *obj) {
    XR_DCHECK(head != NULL, "node_remove: NULL head");
    bool removed = false;
    XrGCObjectNode **p = head;
    while (*p) {
        XrGCObjectNode *node = *p;
        if (node->obj == obj) {
            *p = node->next;
            xr_free(node);
            removed = true;
            continue;
        }
        p = &node->next;
    }
    return removed;
}

static void gc_finalize_registered_objects(XrCoroGC *gc) {
    while (gc->finalize_objects) {
        XrGCObjectNode *node = gc->finalize_objects;
        gc->finalize_objects = node->next;
        XrGCHeader *obj = node->obj;
        if (obj && !(obj->extra & XR_OBJ_DEAD)) {
            obj->extra |= XR_OBJ_DEAD;
            XrGCDestroyFn destroy = get_destroy_func_ext(gc, obj->type);
            if (destroy) {
                destroy(obj, gc);
                gc->finalizer_count++;
            }
        }
        xr_free(node);
    }
}

// Finalize and free all large objects
static void gc_free_large_objects(XrCoroGC *gc) {
    XrGCObjectNode *node = gc->large_objects;
    while (node) {
        XrGCObjectNode *next = node->next;
        XrGCHeader *lo = node->obj;
        if (lo) {
            gc->large_bytes -= lo->objsize;
            if (XR_GC_IS_MMAP(lo)) {
                xr_mem_unmap(lo, lo->objsize);
            } else {
                xr_free(lo);
            }
        }
        xr_free(node);
        node = next;
    }
    gc->large_objects = NULL;
    gc->large_bytes = 0;
}

static bool gc_prepare_nodes(XrCoroGC *gc, uint8_t type, size_t total,
                             XrGCObjectNode **finalize_node, XrGCObjectNode **large_node) {
    *finalize_node = NULL;
    *large_node = NULL;
    if (xr_gc_needs_finalize_ext(gc, type)) {
        *finalize_node = gc_object_node_new(NULL);
        if (!*finalize_node)
            return false;
    }
    if (total > XR_LARGE_OBJECT_THRESHOLD) {
        *large_node = gc_object_node_new(NULL);
        if (!*large_node) {
            if (*finalize_node) {
                xr_free(*finalize_node);
                *finalize_node = NULL;
            }
            return false;
        }
    }
    return true;
}

static void gc_discard_nodes(XrGCObjectNode *finalize_node, XrGCObjectNode *large_node) {
    if (finalize_node)
        xr_free(finalize_node);
    if (large_node)
        xr_free(large_node);
}

static void gc_register_prepared_nodes(XrCoroGC *gc, XrGCHeader *obj, XrGCObjectNode *finalize_node,
                                       XrGCObjectNode *large_node) {
    if (large_node) {
        gc_object_node_push(&gc->large_objects, large_node, obj);
        gc->large_bytes += (int64_t) obj->objsize;
    }
    if (finalize_node) {
        gc_object_node_push(&gc->finalize_objects, finalize_node, obj);
    }
}

static bool gc_register_finalizer_after_inline_alloc(XrCoroGC *gc, XrGCHeader *obj) {
    if (!xr_gc_needs_finalize_ext(gc, obj->type))
        return true;
    XrGCObjectNode *node = gc_object_node_new(obj);
    if (!node) {
        return false;
    }
    gc_object_node_push(&gc->finalize_objects, node, obj);
    XR_OBJ_SET_FLAG(obj, XR_OBJ_HAS_DTOR);
    return true;
}

static void gc_unregister_finalizer(XrCoroGC *gc, XrGCHeader *obj) {
    if (gc)
        (void) gc_object_node_remove(&gc->finalize_objects, obj);
}

static void gc_unregister_large_object(XrCoroGC *gc, XrGCHeader *obj) {
    if (gc)
        (void) gc_object_node_remove(&gc->large_objects, obj);
}

/* ========== Lifecycle ========== */

void xr_coro_gc_destroy(XrCoroGC *gc) {
    if (!gc)
        return;
    XR_DCHECK(!gc->in_gc, "gc_destroy called during GC");

    gc_finalize_registered_objects(gc);
    xr_immix_destroy(&gc->immix);
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
    xr_immix_reset(&gc->immix);
    gc_free_large_objects(gc);
    xr_coro_gc_rc_freelist_destroy(gc);
    xr_cycle_roots_destroy(gc);

    gc_init_runtime_state(gc);
    gc->owner = new_owner;
}

/* ========== Allocation Helpers ========== */

static inline void gc_post_immix_alloc(XrCoroGC *gc, XrGCHeader *obj, uint8_t type,
                                       uint32_t total) {
    (void) gc;
    (void) type;
    XrImmixBlock *block = XR_IMMIX_BLOCK_FROM_PTR(obj);
    block->alloc_count++;
    block->alloc_bytes += (int64_t) total;

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
        gc_unregister_large_object(gc, obj);
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
    /* The blocks themselves live in Immix and are released in bulk when the
     * coroutine's heap is torn down; only the index array is owned here. */
    xr_free(gc->rc_freelist);
    gc->rc_freelist = NULL;
}

/* Maximum recursive destroy depth before switching to deferred mode.
 * Prevents stack overflow on deep data structures (linked lists 10K+
 * nodes, deeply nested trees). When exceeded, child objects are pushed
 * onto gc->deferred_drops and drained iteratively by the top-level call. */
#define XR_DESTROY_DEPTH_LIMIT 64

/* Core destroy logic (shared by top-level and deferred-drain paths). */
static void rc_destroy_one(XrCoroGC *gc, XrGCHeader *obj) {
    XR_DCHECK(obj != NULL, "rc_destroy_one: NULL obj");
    obj->extra |= XR_OBJ_DEAD;

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

/* ========== Drop-Reuse (Perceus-style in-place allocation) ========== */

XR_FUNC XrGCHeader *xr_rc_drop_reuse(XrCoroGC *gc, XrGCHeader *obj) {
    if (!obj)
        return NULL;
    if (!coro_gc_header_is_heap_value(obj))
        return NULL;
    /* Shared / region / managed objects cannot be reused locally. */
    if (obj->extra & (XR_OBJ_REGION | XR_OBJ_MANAGED))
        return NULL;
    if (XR_GC_IS_SHARED(obj)) {
        xr_shared_destroy(obj);
        return NULL;
    }

    /* Attempt to take the last reference. If RC > 1, just decrement and
     * return NULL — the object is still alive. */
    if (!xr_obj_drop_is_last((XrObjHeader *) obj))
        return NULL;

    /* RC reached zero. Run the type destructor so fields are properly
     * released (destructors drop child references). The memory stays. */
    obj->extra |= XR_OBJ_DEAD;
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

    /* Return the raw memory for immediate reuse. The caller will
     * reinitialize the header via xr_rc_alloc_at. Do NOT push to
     * the freelist — the whole point is to skip the freelist. */
    return obj;
}

XR_FUNC XrGCHeader *xr_rc_alloc_at(XrCoroGC *gc, XrGCHeader *token, uint8_t type, size_t size) {
    XR_DCHECK(type < XGC_MAX_TYPES, "xr_rc_alloc_at: invalid type");
    XR_DCHECK(size >= sizeof(XrGCHeader), "xr_rc_alloc_at: size too small");

    size_t total = XGC_ALIGN(size);
    XrGCHeader *obj;

    if (token != NULL && token->objsize >= (uint32_t) total) {
        /* Reuse: the reclaimed block is large enough. Reinitialize header. */
        obj = token;
    } else {
        /* Fallback: token is NULL (object was shared / still alive) or the
         * reclaimed block is too small. Free the token if it exists and
         * allocate fresh. */
        if (token && gc)
            xr_coro_gc_rc_free(gc, token);
        obj = xr_coro_gc_newobj(gc, type, total);
        return obj; /* newobj already initializes the header */
    }

    /* Reinitialize the header for the new type/size. Clear all flags
     * except storage bits that are inherited from the block's origin
     * (shared/normal). Actually, for reused blocks the caller always
     * owns them locally, so clear everything. */
    memset(obj, 0, sizeof(XrGCHeader));
    obj->type = type;
    obj->refcount = 1;
    obj->objsize = (uint32_t) total;

    if (gc)
        gc->object_count++;

    return obj;
}

XrGCHeader *xr_coro_gc_newobj(XrCoroGC *gc, uint8_t type, size_t size) {
    if (!gc)
        return NULL;
    XR_DCHECK(type < XGC_MAX_TYPES, "invalid GC type");
    XR_DCHECK(size >= sizeof(XrGCHeader), "alloc size too small for GC header");
    XR_DCHECK(gc->owner != NULL, "GC has no owner coroutine");

    size_t total = XGC_ALIGN(size);
    XrGCHeader *obj;
    XrGCObjectNode *finalize_node = NULL;
    XrGCObjectNode *large_node = NULL;

    if (!gc_prepare_nodes(gc, type, total, &finalize_node, &large_node))
        return NULL;

    bool use_mmap = false;
    if (total > XR_LARGE_OBJECT_THRESHOLD) {
        if (total >= XR_MMAP_THRESHOLD) {
            // Tier 2: very large — use anonymous mmap (xr_mem_map)
            // to avoid libc heap fragmentation.
            obj = (XrGCHeader *) xr_mem_map(total, XR_MEM_PROT_READ | XR_MEM_PROT_WRITE);
            if (!obj) {
                gc_discard_nodes(finalize_node, large_node);
                return NULL;
            }
            use_mmap = true;
        } else {
            // Tier 1: medium large — use xr_malloc
            obj = (XrGCHeader *) xr_malloc(total);
            if (!obj) {
                gc_discard_nodes(finalize_node, large_node);
                return NULL;
            }
        }
    } else {
        /* RC freelist fast path: reuse a same-size-class block freed by a
         * previous drop-to-zero before falling back to Immix bump. */
        int cls = xr_rc_size_class(total);
        if (cls >= 0 && gc->rc_freelist && gc->rc_freelist[cls]) {
            obj = gc->rc_freelist[cls];
            void **link = (void **) ((char *) obj + sizeof(XrGCHeader));
            gc->rc_freelist[cls] = (XrGCHeader *) *link;
            /* Reused block: its allocation line is still occupied. type/refcount/extra
             * are reset below; XR_OBJ_DEAD is cleared by extra = 0. */
        } else {
            obj = (XrGCHeader *) xr_immix_alloc(&gc->immix, total);
            if (!obj) {
                gc_discard_nodes(finalize_node, large_node);
                return NULL;
            }
            gc_post_immix_alloc(gc, obj, type, (uint32_t) total);
        }
    }

    obj->type = type;
    obj->objsize = (uint32_t) total;
    obj->extra = 0;  // Always clear extra (Immix memory may be uninitialized)
    obj->_rsv = 0;
    /* RC: a freshly allocated object has exactly one owning reference (its
     * definition site). dup/drop are 1-based, so initialize to 1. Immix
     * memory is reused/uninitialized, so this must be set explicitly. */
    obj->refcount = 1;
    if (use_mmap)
        XR_GC_SET_MMAP(obj);
    if (finalize_node)
        XR_OBJ_SET_FLAG(obj, XR_OBJ_HAS_DTOR);

    gc_register_prepared_nodes(gc, obj, finalize_node, large_node);

    gc_update_alloc_stats(gc, (uint32_t) total);

    return obj;
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

    printf("Object count:   %u\n", gc->object_count);

    size_t large_count = 0;
    for (XrGCObjectNode *node = gc->large_objects; node; node = node->next)
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
// Only sets line occupancy bits; stats are handled inline by JIT.
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
//   2. finalizer registration for objects that need teardown hooks
//   3. stats update (totalbytes, object_count)
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
    XR_CHECK(gc_register_finalizer_after_inline_alloc(gc, obj),
             "jit_alloc_post: finalizer registration failed");
    gc_update_alloc_stats(gc, total);
}
