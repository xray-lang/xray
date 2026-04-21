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
#include "xcoro_gc_traverse.h"
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
#include "../../runtime/xexec_state.h"
#include "../value/xstruct_layout.h"
#include "../class/xclass.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include "../../base/xmalloc.h"
#include "xstackmap.h"  // XrStackMapTable, XrStackMapEntry
#include "xbc_stackmap.h"  // XrBcStackMap, bytecode precise GC scanning
#include <pthread.h>

/* ========== GC Struct Two-Level Pool ========== */
/*
 * L1: per-Worker gc_free_list (lock-free, max 32).
 * L2: global mutex-protected stack (max 256).
 *
 * L1 miss → L2 → xr_malloc.
 * L1 full → L2 → xr_free if L2 full.
 * Worker exit flushes L1 → L2 (xr_coro_gc_flush_pool).
 */
#define XR_GC_POOL_L1_MAX   32
#define XR_GC_POOL_L2_MAX  256

static pthread_mutex_t g_gc_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static XrCoroGC *g_gc_pool_head = NULL;
static int g_gc_pool_count = 0;

static XrCoroGC* gc_pool_l2_pop(void) {
    pthread_mutex_lock(&g_gc_pool_mu);
    XrCoroGC *gc = g_gc_pool_head;
    if (gc) {
        g_gc_pool_head = *(XrCoroGC**)gc;
        g_gc_pool_count--;
    }
    pthread_mutex_unlock(&g_gc_pool_mu);
    return gc;
}

static bool gc_pool_l2_push(XrCoroGC *gc) {
    pthread_mutex_lock(&g_gc_pool_mu);
    if (g_gc_pool_count >= XR_GC_POOL_L2_MAX) {
        pthread_mutex_unlock(&g_gc_pool_mu);
        return false;
    }
    *(XrCoroGC**)gc = g_gc_pool_head;
    g_gc_pool_head = gc;
    g_gc_pool_count++;
    pthread_mutex_unlock(&g_gc_pool_mu);
    return true;
}

/* ========== Helper Functions ========== */

// Bitmap: types that contain GC references (need gray list traversal)
static const uint64_t HAS_REFS_BITMAP =
    (1ULL << XR_TARRAY) | (1ULL << XR_TARRAY_SLICE) |
    (1ULL << XR_TMAP) | (1ULL << XR_TSET) | (1ULL << XR_TJSON) |
    (1ULL << XR_TFUNCTION) | (1ULL << XR_TINSTANCE) |
    (1ULL << XR_TITERATOR) |
    (1ULL << XR_TCELL) | (1ULL << XR_TMODULE) |
    (1ULL << XR_TBOUND_METHOD) |
    (1ULL << XR_TEXCEPTION) |
    (1ULL << XR_TERROR); // has message/file/stackTrace/userData

static inline bool xr_gc_has_refs(uint8_t type) {
    return type < XR_NATIVE_TYPE_MAX && (HAS_REFS_BITMAP & (1ULL << type));
}

// Bitmap: types that need finalization (have external malloc'd resources)
static const uint64_t NEEDS_FINALIZE_BITMAP =
    (1ULL << XR_TARRAY) | (1ULL << XR_TMAP) | (1ULL << XR_TSET) |
    (1ULL << XR_TSTRINGBUILDER) |
    (1ULL << XR_TREGEX) | (1ULL << XR_TCOROUTINE) |
    (1ULL << XR_TLOGGER) | (1ULL << XR_TTASK);

static inline bool xr_gc_needs_finalize(uint8_t type) {
    return type < XR_NATIVE_TYPE_MAX && (NEEDS_FINALIZE_BITMAP & (1ULL << type));
}

// Get global destroy function for type (const table defined in xgc.c)
static inline XrGCDestroyFn get_destroy_func(uint8_t type) {
    return (type < XGC_MAX_TYPES) ? g_destroy_funcs[type] : NULL;
}

/* ========== Debug Invariant Forward Declaration ========== */
#if XR_GC_DEBUG
static void xr_gc_verify_invariants(XrCoroGC *gc);
#else
static inline void xr_gc_verify_invariants(XrCoroGC *gc) { (void)gc; }
#endif

/*
 * Reset GC runtime state fields to initial values.
 * Shared by xr_coro_gc_create and xr_coro_gc_reset.
 * Does NOT touch: immix heap, gray list buffers, shared_refs buffers,
 * tuning params (gc_pause, gc_stepmul), or owner pointer.
 */
static void gc_init_runtime_state(XrCoroGC *gc) {
    gc->gcstate = XGC_PAUSE;
    gc->currentwhite = XGC_WHITE0;
    gc->gc_mode = XGC_MODE_GEN;
    gc->GCmarked = 0;
    gc->GCest = 0;
    gc->young_promoted = 0;
    gc->totalbytes = 0;
    gc->large_bytes = 0;
    gc->sweep_phase = XGC_SWEEP_DONE;
    gc->sweep_block = NULL;
    gc->alloc_since_gc = 0;
    gc->in_gc = 0;
    gc->gc_disabled = 0;
    gc->gc_requested = 0;
    gc->gc_count = 0;
    gc->object_count = 0;
    gc->gc_time_ns = 0;
    gc->last_gc_time_ns = 0;
    gc->gc_cycle_start_ns = 0;
    gc->finalizer_count = 0;
}

/* ========== Coroutine GC Lifecycle ========== */

XrCoroGC* xr_coro_gc_create(struct XrCoroutine *coro, const XrCoroGCConfig *config) {
    XR_DCHECK(coro != NULL, "gc_create: NULL coroutine");
    XrCoroGC *gc = NULL;

    // Fast path: L1 per-Worker free list (no lock)
    XrWorker *w = xr_current_worker();
    if (w && w->p.gc_free_list) {
        gc = w->p.gc_free_list;
        w->p.gc_free_list = *(XrCoroGC**)gc;
        w->p.gc_free_count--;
    } else {
        // L2 global pool (mutex)
        gc = gc_pool_l2_pop();
        if (!gc) {
            gc = (XrCoroGC*)xr_malloc(sizeof(XrCoroGC));
        }
    }
    if (!gc) return NULL;

    memset(gc, 0, sizeof(XrCoroGC));

    // Initialize Immix heap
    xr_immix_init(&gc->immix);

    // Gray lists need explicit init (sets items=NULL which memset already did,
    // but xr_gclist_init is the canonical initializer)
    xr_gclist_init(&gc->gray);
    xr_gclist_init(&gc->grayagain);
    xr_gclist_init(&gc->weak);

    gc_init_runtime_state(gc);

    int64_t threshold = (config && config->gc_threshold > 0)
        ? (int64_t)config->gc_threshold
        : (int64_t)XR_SPAWN_CORO_GC_THRESHOLD;
    gc->GCdebt = -threshold;

    gc->gc_pause = config && config->gc_pause > 0
        ? config->gc_pause
        : XR_SPAWN_CORO_GC_PAUSE;
    gc->gc_stepmul = config && config->gc_stepmul > 0
        ? config->gc_stepmul
        : XR_SPAWN_CORO_GC_STEPMUL;

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
    XrImmixBlock *blists[] = {
        gc->immix.full_blocks,
        gc->immix.recycle_blocks,
        gc->immix.current_block,
        gc->immix.old_blocks
    };
    for (int i = 0; i < 4; i++) {
        for (XrImmixBlock *b = blists[i]; b; b = b->next) {
            if (!b->has_finalizers) continue;
            for (XrGCHeader *obj = b->local_allgc; obj; obj = obj->gc_next) {
                if (xr_gc_needs_finalize(obj->type)) {
                    XrGCDestroyFn destroy = get_destroy_func(obj->type);
                    if (destroy) destroy(obj, gc);
                }
            }
            if (i == 2) break;  // current_block is single, not a list
        }
    }
}

// Finalize and free all large objects
static void gc_free_large_objects(XrCoroGC *gc) {
    XrGCHeader *lo = gc->large_objects;
    while (lo) {
        XrGCHeader *next = lo->gc_next;
        if (xr_gc_needs_finalize(lo->type)) {
            XrGCDestroyFn destroy = get_destroy_func(lo->type);
            if (destroy) destroy(lo, gc);
        }
        gc->large_bytes -= lo->objsize;
        if (XR_GC_IS_MMAP(lo)) {
            munmap(lo, lo->objsize);
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
        if (new_refc == 0) xr_shared_destroy(obj);
    }
}

/* ========== Lifecycle ========== */

void xr_coro_gc_destroy(XrCoroGC *gc) {
    if (!gc) return;
    XR_DCHECK(!gc->in_gc, "gc_destroy called during GC");

    gc_free_root_callbacks(gc);
    gc_finalize_immix_objects(gc);
    xr_immix_destroy(&gc->immix);
    gc_free_large_objects(gc);

    xr_gclist_destroy(&gc->gray);
    xr_gclist_destroy(&gc->grayagain);
    xr_gclist_destroy(&gc->weak);

    gc_decref_all_shared(gc);
    if (gc->shared_refs) xr_free(gc->shared_refs);
    if (gc->prev_shared_refs) xr_free(gc->prev_shared_refs);

    // Recycle: try L1 (per-Worker), then L2 (global), then free
    XrWorker *w = xr_current_worker();
    if (w && w->p.gc_free_count < XR_GC_POOL_L1_MAX) {
        *(XrCoroGC**)gc = w->p.gc_free_list;
        w->p.gc_free_list = gc;
        w->p.gc_free_count++;
    } else if (!gc_pool_l2_push(gc)) {
        xr_free(gc);
    }
}

// Flush a per-worker GC struct free list (L1) to the global pool (L2).
// Structs that don't fit in L2 are freed immediately.
void xr_coro_gc_flush_pool(XrCoroGC **free_list, int *count) {
    XR_DCHECK(free_list != NULL, "flush_pool: NULL free_list");
    XR_DCHECK(count != NULL, "flush_pool: NULL count");
    while (*free_list) {
        XrCoroGC *gc = *free_list;
        *free_list = *(XrCoroGC**)gc;
        if (!gc_pool_l2_push(gc)) {
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
    if (!gc) return;
    XR_DCHECK(new_owner != NULL, "gc_reset: NULL new_owner");
    XR_DCHECK(!gc->in_gc, "gc_reset called during GC");

    gc_free_root_callbacks(gc);
    gc_finalize_immix_objects(gc);
    xr_immix_reset(&gc->immix);
    gc_free_large_objects(gc);

    xr_gclist_reset(&gc->gray);
    xr_gclist_reset(&gc->grayagain);
    xr_gclist_reset(&gc->weak);

    gc_decref_all_shared(gc);
    gc->shared_refs_count = 0;
    gc->prev_shared_refs_count = 0;

    // Reset runtime state (keep tuning params and shared_refs buffers)
    gc_init_runtime_state(gc);
    gc->GCdebt = -(int64_t)XR_SPAWN_CORO_GC_THRESHOLD;
    gc->owner = new_owner;
}

/* ========== Allocation Helpers ========== */

/*
 * Link Immix object to block's local_allgc list and mark allocation lines.
 * Shared by xr_coro_gc_newobj (interpreter) and xr_jit_alloc_post (JIT).
 */
static inline void gc_post_immix_alloc(XrGCHeader *obj, uint8_t type,
                                       uint32_t total) {
    XrImmixBlock *block = XR_IMMIX_BLOCK_FROM_PTR(obj);
    obj->gc_next = block->local_allgc;
    block->local_allgc = obj;
    block->alloc_count++;
    block->alloc_bytes += (int64_t)total;
    if (xr_gc_needs_finalize(type))
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
 * Update GC allocation statistics after object creation.
 * Shared by xr_coro_gc_newobj and xr_jit_alloc_post.
 */
static inline void gc_update_alloc_stats(XrCoroGC *gc, uint32_t total) {
    gc->totalbytes += (int64_t)total;
    gc->object_count++;
    XR_DCHECK(gc->totalbytes >= 0, "totalbytes underflow");
    if (gc->gc_disabled == 0) {
        gc->GCdebt += (int64_t)total;
        gc->alloc_since_gc += total;
        if (gc->GCdebt > 0 && !gc->in_gc)
            gc->gc_requested = 1;
    }
}

/* ========== Allocation ========== */

XrGCHeader* xr_coro_gc_newobj(XrCoroGC *gc, uint8_t type, size_t size) {
    if (!gc) return NULL;
    XR_DCHECK(type < XGC_MAX_TYPES, "invalid GC type");
    XR_DCHECK(size >= sizeof(XrGCHeader), "alloc size too small for GC header");
    XR_DCHECK(gc->owner != NULL, "GC has no owner coroutine");

    size_t total = XGC_ALIGN(size);
    XrGCHeader *obj;

    bool use_mmap = false;
    if (total > XR_LARGE_OBJECT_THRESHOLD) {
        if (total >= XR_MMAP_THRESHOLD) {
            // Tier 2: very large — use mmap to avoid libc heap fragmentation
            obj = (XrGCHeader*)mmap(NULL, total, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (obj == MAP_FAILED) return NULL;
            use_mmap = true;
        } else {
            // Tier 1: medium large — use xr_malloc
            obj = (XrGCHeader*)xr_malloc(total);
            if (!obj) return NULL;
        }
        obj->gc_next = gc->large_objects;
        gc->large_objects = obj;
        gc->large_bytes += (int64_t)total;
    } else {
        obj = (XrGCHeader*)xr_immix_alloc(&gc->immix, total);
        if (!obj) return NULL;
        gc_post_immix_alloc(obj, type, (uint32_t)total);
    }

    obj->type = type;
    obj->objsize = (uint32_t)total;
    obj->extra = 0;  // Always clear extra (Immix memory may be uninitialized)
    if (use_mmap) XR_GC_SET_MMAP(obj);

    // New objects born with currentwhite (in young blocks or large list)
    // INVARIANT 3: new objects carry currentwhite
    obj->marked = gc->currentwhite;
    XR_DCHECK(xr_gc_iswhite(obj), "new object must be white");

    gc_update_alloc_stats(gc, (uint32_t)total);

#if XR_GC_STRESS
    // Stress mode: trigger a GC step on every allocation to expose
    // write barrier bugs that only manifest under tight GC pressure.
    if (gc->gc_disabled == 0 && !gc->in_gc)
        xr_coro_gc_step(gc);
#endif

    return obj;
}

/* ========== Shared Object Refcount Tracking ========== */

static int ptr_compare(const void *a, const void *b) {
    uintptr_t pa = (uintptr_t)*(XrGCHeader**)a;
    uintptr_t pb = (uintptr_t)*(XrGCHeader**)b;
    return (pa > pb) - (pa < pb);
}

// Sort array and remove duplicates in-place. Returns new count.
static int sort_and_dedup(XrGCHeader **arr, int count) {
    if (count <= 1) return count;
    qsort(arr, (size_t)count, sizeof(XrGCHeader*), ptr_compare);
    int w = 1;
    for (int r = 1; r < count; r++) {
        if (arr[r] != arr[r - 1]) {
            arr[w++] = arr[r];
        }
    }
    return w;
}

// Binary search in sorted array.
static bool sorted_contains(XrGCHeader **arr, int count, XrGCHeader *target) {
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (arr[mid] == target) return true;
        if ((uintptr_t)arr[mid] < (uintptr_t)target) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

// Begin a new GC cycle: rotate shared_refs → prev_shared_refs
static void gc_shared_refs_begin(XrCoroGC *gc) {
    XR_DCHECK(gc != NULL, "gc_shared_refs_begin: NULL gc");
    if (gc->prev_shared_refs) xr_free(gc->prev_shared_refs);
    gc->prev_shared_refs = gc->shared_refs;
    gc->prev_shared_refs_count = gc->shared_refs_count;
    gc->shared_refs = NULL;
    gc->shared_refs_count = 0;
    gc->shared_refs_capacity = 0;
}

// End a GC cycle: decref shared objects that were in prev but not in current.
// Sort current shared_refs then binary-search for each prev entry: O(n log n).
static void gc_shared_refs_end(XrCoroGC *gc) {
    if (!gc->prev_shared_refs || gc->prev_shared_refs_count == 0) return;
    // Sort + dedup current refs for efficient lookup
    gc->shared_refs_count = sort_and_dedup(gc->shared_refs, gc->shared_refs_count);
    for (int i = 0; i < gc->prev_shared_refs_count; i++) {
        XrGCHeader *obj = gc->prev_shared_refs[i];
        if (!sorted_contains(gc->shared_refs, gc->shared_refs_count, obj)) {
            int new_refc = xr_shared_decref(obj);
            if (new_refc == 0) xr_shared_destroy(obj);
        }
    }
    xr_free(gc->prev_shared_refs);
    gc->prev_shared_refs = NULL;
    gc->prev_shared_refs_count = 0;
}

/* ========== Mark Functions ========== */

static void reallymarkobject(XrCoroGC *gc, XrGCHeader *obj) {
    XR_DCHECK(obj != NULL, "marking NULL object");
    XR_DCHECK(xr_gc_iswhite(obj), "reallymarkobject called on non-white object");
    XR_DCHECK(!XR_GC_IS_SHARED(obj), "shared object in tri-color mark");
    xr_gc_white2gray(obj);
    // Tag block so sweep_block can skip all-dead blocks without traversal.
    // Large objects (>4KB) are malloc'd, not in any Immix block.
    if (obj->objsize <= XR_LARGE_OBJECT_THRESHOLD) {
        XR_IMMIX_BLOCK_FROM_PTR(obj)->has_marked = 1;
    }
    if (xr_gc_has_refs(obj->type)) {
        xr_gclist_push(&gc->gray, obj);
    } else {
        xr_gc_gray2black(obj);
    }
}

// Record a shared object reference for this coroutine's GC cycle.
// Appends without dedup (O(1)); dedup happens in gc_shared_refs_end via sort.
static void gc_record_shared_ref(XrCoroGC *gc, XrGCHeader *obj) {
    XR_DCHECK(gc != NULL, "gc_record_shared_ref: NULL gc");
    XR_DCHECK(obj != NULL, "gc_record_shared_ref: NULL obj");
    XR_DCHECK(XR_GC_IS_SHARED(obj), "gc_record_shared_ref: not a shared object");
    if (gc->shared_refs_count >= gc->shared_refs_capacity) {
        int newcap = gc->shared_refs_capacity ? gc->shared_refs_capacity * 2 : 8;
        XrGCHeader **newrefs = (XrGCHeader**)xr_realloc(gc->shared_refs, newcap * sizeof(XrGCHeader*));
        if (!newrefs) return;
        gc->shared_refs = newrefs;
        gc->shared_refs_capacity = newcap;
    }
    gc->shared_refs[gc->shared_refs_count++] = obj;
}

void xr_coro_gc_markobject(XrCoroGC *gc, XrGCHeader *obj) {
    if (!obj) return;
    // Shared objects: track reference but don't mark (managed by refcount)
    if (XR_GC_IS_SHARED(obj)) {
        /* Global pool strings are owned by XrGlobalStringPool, not refcounted.
         * Skip shared_refs tracking to avoid refcount underflow. */
        if (XR_GC_GET_TYPE(obj) == XR_TSTRING && (obj->extra & STR_FLAG_GLOBAL)) {
            XR_STR_SET_ACCESSED((XrString *)obj);
            return;
        }
        gc_record_shared_ref(gc, obj);
        return;
    }
    if (xr_gc_iswhite(obj)) {
        reallymarkobject(gc, obj);
    } else if (xr_gc_has_refs(obj->type)) {
        // Defensive traversal for external objects not managed by this coro GC.
        // After scheme C, closures are primarily on Immix heap, but fixed GC
        // fallback objects (e.g. deep-copied closures when dst_coro_gc is NULL)
        // may still appear. Their children on Immix heap must be traversed.
        xr_gc_traverse_object(gc, obj);
    }
}

void xr_coro_gc_markvalue(XrCoroGC *gc, XrValue value) {
    if (XR_VALUE_NEEDS_GC(value)) {
        XrGCHeader *obj = XR_VALUE_GCPTR(value);
        xr_coro_gc_markobject(gc, obj);
    }
}

/* ========== Mark Roots ========== */

/*
 * Scan struct string fields in per-frame struct_areas.
 * Struct data is raw bytes (not XrValues), so GC stack scanning
 * doesn't see string pointers embedded inside. We iterate all
 * struct_area data, find XrClass* headers with struct_layout,
 * and mark any XR_NATIVE_STRING fields.
 */
static void mark_struct_string_fields(XrCoroGC *gc, XrVMContext *vm_ctx) {
    if (!vm_ctx->struct_areas) return;
    int frame_count = vm_ctx->frame_count;
    XrBcCallFrame *frames = vm_ctx->frames;
    if (!frames) return;

    for (int fi = 0; fi < frame_count; fi++) {
        if (fi >= vm_ctx->struct_areas_cap) break;
        uint8_t *area = vm_ctx->struct_areas[fi];
        if (!area) continue;

        XrBcCallFrame *f = &frames[fi];
        if (!f->closure || !f->closure->proto) continue;
        uint16_t area_size = f->closure->proto->struct_area_size;
        if (area_size == 0) continue;

        // Scan stack values in this frame to find live struct refs
        size_t base = f->base_offset;
        size_t frame_end;
        if (fi + 1 < frame_count) {
            frame_end = frames[fi + 1].base_offset;
            if (f->closure->proto) {
                size_t proto_end = base + (size_t)f->closure->proto->maxstacksize;
                if (proto_end > frame_end) frame_end = proto_end;
            }
        } else {
            frame_end = vm_ctx->stack_top ? (size_t)(vm_ctx->stack_top - vm_ctx->stack) : base;
            if (f->closure->proto) {
                size_t proto_end = base + (size_t)f->closure->proto->maxstacksize;
                if (proto_end > frame_end) frame_end = proto_end;
            }
        }

        XrValue *stack = vm_ctx->stack;
        for (size_t si = base; si < frame_end; si++) {
            XrValue v = stack[si];
            if (v.tag != XR_TAG_STRUCT_REF || v.ext != 0) continue;
            uint8_t *sptr = (uint8_t*)v.ptr;
            if (!sptr) continue;
            // Verify pointer is within this frame's struct_area
            if (sptr < area || sptr >= area + area_size) continue;
            XrClass *cls = *(XrClass**)sptr;
            if (!cls || !cls->struct_layout) continue;
            XrStructLayout *layout = cls->struct_layout;
            for (int i = 0; i < layout->field_count; i++) {
                if (layout->fields[i].native_type == XR_NATIVE_STRING) {
                    XrString *s = *(XrString**)(sptr + 8 + layout->fields[i].offset);
                    if (s) xr_coro_gc_markobject(gc, (XrGCHeader*)s);
                }
            }
        }
    }
}

/*
 * Re-mark coroutine stack and frame roots.
 * Called from markroots (initial scan) and atomic (re-scan before white flip).
 *
 * WHY THIS DESIGN:
 *   Objects created during PROPAGATE are born with old currentwhite.
 *   After the white flip in atomic, they become deadwhite and would be
 *   swept even though they're still live on the stack. Re-marking the
 *   stack in atomic catches these objects (same as Lua's markobject(g,L)).
 */
static void mark_coro_roots(XrCoroGC *gc) {
    XR_DCHECK(gc != NULL, "mark_coro_roots: NULL gc");
    struct XrCoroutine *coro = gc->owner;
    if (!coro) return;

    // vm_ctx is the single source of truth for stack/frames
    XrValue *stack_top = coro->vm_ctx.stack_top;
    size_t actual_size = stack_top ? (size_t)(stack_top - coro->vm_ctx.stack) : 0;

    int frame_count = coro->vm_ctx.frame_count;
    XrBcCallFrame *frames = coro->vm_ctx.frames;

    if (frames && frame_count > 0) {
        // Extend actual_size to cover every Xray frame's full register file.
        // stack_top is often not updated before GC safepoints, and the topmost
        // frame is frequently a C frame (e.g. gc.collect) with no proto, so
        // scanning only top_frame->base_offset + maxstacksize misses the caller's
        // high-numbered live registers (e.g. captured strings and arrays).
        for (int fi = 0; fi < frame_count; fi++) {
            XrBcCallFrame *f2 = &frames[fi];
            if (f2->closure && f2->closure->proto) {
                size_t proto_end = (size_t)f2->base_offset +
                                   (size_t)f2->closure->proto->maxstacksize;
                if (proto_end > actual_size) actual_size = proto_end;
            }
        }
        // Also ensure the top frame itself is covered (handles the C-frame case
        // where its base_offset alone might be larger than stack_top).
        XrBcCallFrame *top_frame = &frames[frame_count - 1];
        if ((size_t)top_frame->base_offset > actual_size) {
            actual_size = (size_t)top_frame->base_offset;
        }
    }

    // Per-frame stack scanning: zero extra memory, single pass.
    // Slots partitioned by frame boundaries so each slot is scanned exactly once.
    if (coro->vm_ctx.stack && actual_size > 0) {
        XrValue *stack = coro->vm_ctx.stack;

        if (frames && frame_count > 0) {
            for (int fi = 0; fi < frame_count; fi++) {
                XrBcCallFrame *f = &frames[fi];
                size_t base = f->base_offset;

                // Frame extent: [base, frame_end)
                size_t frame_end;
                if (fi + 1 < frame_count) {
                    frame_end = frames[fi + 1].base_offset;
                    // The callee's base_offset equals the INVOKE/CALL A register,
                    // which is often much smaller than this frame's maxstacksize.
                    // Extend to cover all live registers of this Xray frame so
                    // that captured variables (e.g. arr, captured strings) stored
                    // in high-numbered registers are not missed by GC.
                    if (f->closure && f->closure->proto) {
                        size_t proto_end = base + (size_t)f->closure->proto->maxstacksize;
                        if (proto_end > frame_end) frame_end = proto_end;
                    }
                } else {
                    frame_end = actual_size;
                }
                if (frame_end > actual_size) frame_end = actual_size;
                if (base >= frame_end) continue;

                // C frames or missing proto: conservative scan
                if ((f->call_status & XR_CALL_C) || !f->closure || !f->closure->proto) {
                    for (size_t i = base; i < frame_end; i++)
                        xr_coro_gc_markvalue(gc, stack[i]);
                    continue;
                }

                // Try precise scan using bytecode stackmap.
                // If the current PC has a safepoint entry, scan only live slots.
                // Otherwise fall back to conservative (scan all slots).
                XrProto *proto = f->closure->proto;
                const XrBcStackMap *bcmap = (const XrBcStackMap *)proto->bc_stackmap;
                const XrBcStackMapEntry *sme = NULL;
                if (bcmap && f->pc) {
                    uint32_t pc_off = (uint32_t)(f->pc - (XrInstruction*)proto->code.data);
                    sme = xr_bc_stackmap_lookup(bcmap, pc_off);
                }
                if (sme) {
                    // Precise scan: only mark live slots
                    uint32_t nslots = (uint32_t)(frame_end - base);
                    for (uint32_t w = 0; w < sme->num_words; w++) {
                        uint64_t bits = bcmap->bitmap_pool[sme->bitmap_offset + w];
                        while (bits) {
                            int bit = __builtin_ctzll(bits);
                            uint32_t slot = w * 64 + (uint32_t)bit;
                            if (slot < nslots) {
                                xr_coro_gc_markvalue(gc, stack[base + slot]);
                            }
                            bits &= bits - 1;
                        }
                    }
                } else {
                    // Conservative scan: mark all slots in frame
                    for (size_t i = base; i < frame_end; i++)
                        xr_coro_gc_markvalue(gc, stack[i]);
                }
            }

            // Slots before frame[0].base_offset (e.g. varargs area)
            if (frames[0].base_offset > 0) {
                for (size_t i = 0; i < (size_t)frames[0].base_offset && i < actual_size; i++)
                    xr_coro_gc_markvalue(gc, stack[i]);
            }
        } else {
            // No frames: mark all conservatively
            for (size_t i = 0; i < actual_size; i++)
                xr_coro_gc_markvalue(gc, stack[i]);
        }
    }

    // Mark string pointers inside struct fields (struct data is raw bytes,
    // not XrValues, so normal stack scanning doesn't reach them)
    mark_struct_string_fields(gc, &coro->vm_ctx);

    // Mark entry closure
    if (coro->entry_type == XR_CORO_ENTRY_CLOSURE && coro->entry.closure) {
        xr_coro_gc_markobject(gc, (XrGCHeader*)coro->entry.closure);
    }

    // Mark shared array values (main coroutine only).
    // Shared variables (OP_SETSHARED) reference objects on main_coro's GC heap.
    // Without this, GC would collect them after stack reset (e.g. between @test runs).
    if (xr_coro_flags_has(coro, XR_CORO_FLG_MAIN) && coro->isolate) {
        XrVMState *vm = xr_isolate_get_vm_state(coro->isolate);
        if (vm) {
            for (int i = 0; i < vm->shared.count; i++) {
                xr_coro_gc_markvalue(gc, vm->shared.data[i]);
            }
        }
    }

    // Mark task: allocated on executor's heap via xr_alloc(executor, ..., XR_TTASK).
    // Without this root, per-coro GC collects the task while the coroutine runs,
    // causing use-after-free in worker_handle_vm_result → xr_task_complete.
    if (coro->task) {
        xr_coro_gc_markobject(gc, (XrGCHeader*)coro->task);
    }

    // Mark coroutine result/error/pending_closure_result
    xr_coro_gc_markvalue(gc, coro->result);
    xr_coro_gc_markvalue(gc, coro->error);
    xr_coro_gc_markvalue(gc, coro->pending_closure_result);

    // Mark send_value: kept alive while coroutine is blocked on channel send
    if (coro->wait_channel && coro->wait_send) {
        xr_coro_gc_markvalue(gc, coro->send_value);
    }

    // Mark pending cfunc_result in call frames
    if (frames) {
        for (int i = 0; i < frame_count; i++) {
            XrBcCallFrame *frame = &frames[i];
            if ((frame->call_status & XR_CALL_C) && frame->u.c.has_cfunc_result) {
                xr_coro_gc_markvalue(gc, frame->u.c.cfunc_result);
            }
            if (frame->closure) {
                xr_coro_gc_markobject(gc, (XrGCHeader*)frame->closure);
            }
            // frame->context removed: context chain no longer used.
        }
    }

    // Mark current JIT frame's closure and proto as GC roots.
    // jit_ctx->call_closure is not a spill slot and not tracked by safepoint
    // bitmaps, so it must be rooted explicitly.  Without this, a GC triggered
    // inside a CALL_C helper (e.g. xr_jit_closure_new → xr_coro_gc_newobj) may
    // collect the live closure and recycle its memory, causing use-after-free.
    if (coro->jit_ctx && coro->jit_ctx->call_closure)
        xr_coro_gc_markobject(gc, (XrGCHeader*)coro->jit_ctx->call_closure);

    // Mark JIT GC stack map roots (compile-time bitmap)
    // Innermost frame: scan registers (from safepoint_saved_sp) + spill slots
    // Caller frames: scan spill slots only (PTR regs written back before calls)
    if (coro->jit_ctx && coro->jit_ctx->active_stack_map) {
        // alloc_regs[15]=x21, [16]=x22, ..., [21]=x27
        // x19 is CORO_REG, x20 is SAFEPT_PAGE_REG — neither is in alloc_regs.
        static const int callee_saved_offsets[] = {
            32,  // [15] x21 (STP x21,x22 at [FP+32])
            40,  // [16] x22
            48,  // [17] x23 (STP x23,x24 at [FP+48])
            56,  // [18] x24
            64,  // [19] x25 (STP x25,x26 at [FP+64])
            72,  // [20] x26
            80,  // [21] x27 (STP x27,x28 at [FP+80])
        };

        // --- Scan innermost frame (where safepoint/call_c was triggered) ---
        uint32_t sid = coro->jit_ctx->active_safepoint_id;
        XrStackMapTable *table = (XrStackMapTable *)coro->jit_ctx->active_stack_map;
        if (sid < table->count) {
            XrStackMapEntry *entry = &table->entries[sid];

            // Scan registers from bitmap (innermost frame has full reg access)
            uint32_t rbits = entry->reg_bitmap;
            while (rbits) {
                int idx = __builtin_ctz(rbits);
                void *ptr = NULL;

                if (idx < 15 && coro->jit_ctx->safepoint_saved_sp) {
                    // Caller-saved x1-x15: from safepoint stub's saved area
                    int64_t *saved = (int64_t *)coro->jit_ctx->safepoint_saved_sp;
                    ptr = (void *)saved[idx];
                } else if (idx >= 15 && idx <= 21 && coro->jit_ctx->jit_frame_sp) {
                    // Callee-saved x21-x27: from JIT frame
                    int cs_off = callee_saved_offsets[idx - 15];
                    ptr = (void *)*(int64_t *)((char *)coro->jit_ctx->jit_frame_sp + cs_off);
                }

                if (ptr) {
                    xr_coro_gc_markobject(gc, (XrGCHeader *)ptr);
                }
                rbits &= rbits - 1;
            }

            // Scan spill slots (SPILL_BASE = 176)
            uint32_t sbits = entry->spill_bitmap;
            if (sbits && coro->jit_ctx->jit_frame_sp) {
                while (sbits) {
                    int idx = __builtin_ctz(sbits);
                    void *ptr = (void *)*(int64_t *)((char *)coro->jit_ctx->jit_frame_sp + 176 + idx * 8);
                    if (ptr) {
                        xr_coro_gc_markobject(gc, (XrGCHeader *)ptr);
                    }
                    sbits &= sbits - 1;
                }
            }
        }

        // --- Walk FP chain for continuous JIT-to-JIT calls (e.g. self-recursion) ---
        // Self-recursive calls don't push to jit_frame_stack; instead the
        // ARM64 FP chain links all recursive frames. Walk up from innermost
        // FP, scanning each valid JIT frame's spill slots + callee-saved regs.
        if (coro->jit_ctx->jit_frame_sp) {
            void *walk_fp = *(void **)coro->jit_ctx->jit_frame_sp;  // caller's FP
            int walk_depth = 0;
            while (walk_fp && walk_depth < 1024) {
                XrStackMapTable *wt = *(XrStackMapTable **)((char *)walk_fp + 160);
                if (!wt || (uintptr_t)wt < 0x1000 || ((uintptr_t)wt & 7) != 0) break;
                if (wt->magic != XR_STACK_MAP_MAGIC) break;

                uint32_t wsid = *(uint32_t *)((char *)walk_fp + 168);
                if (wsid >= wt->count) break;

                XrStackMapEntry *we = &wt->entries[wsid];

                // Scan callee-saved regs
                uint32_t wrbits = we->reg_bitmap;
                while (wrbits) {
                    int idx = __builtin_ctz(wrbits);
                    if (idx >= 15 && idx <= 21) {
                        int cs_off = callee_saved_offsets[idx - 15];
                        void *ptr = (void *)*(int64_t *)((char *)walk_fp + cs_off);
                        if (ptr) xr_coro_gc_markobject(gc, (XrGCHeader *)ptr);
                    }
                    wrbits &= wrbits - 1;
                }

                // Scan spill slots
                uint32_t wsbits = we->spill_bitmap;
                while (wsbits) {
                    int idx = __builtin_ctz(wsbits);
                    void *ptr = (void *)*(int64_t *)((char *)walk_fp + 176 + idx * 8);
                    if (ptr) xr_coro_gc_markobject(gc, (XrGCHeader *)ptr);
                    wsbits &= wsbits - 1;
                }

                walk_fp = *(void **)walk_fp;  // next caller's FP
                walk_depth++;
            }
        }

        // --- Scan caller frames from jit_frame_stack ---
        // Each entry is a caller's FP, pushed before cross-function calls.
        // The caller wrote PTR reg values to spill slots before the call,
        // and stored safepoint_id + stack_map_ptr in its frame.
        uint32_t depth = coro->jit_ctx->jit_frame_depth;
        if (depth > XR_JIT_MAX_FRAME_DEPTH) depth = XR_JIT_MAX_FRAME_DEPTH;
        for (uint32_t d = 0; d < depth; d++) {
            void *caller_fp = coro->jit_ctx->jit_frame_stack[d];
            if (!caller_fp) continue;

            // Read stack_map_ptr from caller's frame [FP+160]
            XrStackMapTable *caller_table = *(XrStackMapTable **)((char *)caller_fp + 160);
            if (!caller_table || (uintptr_t)caller_table < 0x1000 || ((uintptr_t)caller_table & 7) != 0) continue;
            if (caller_table->magic != XR_STACK_MAP_MAGIC) continue;

            // Read safepoint_id from caller's frame [FP+168]
            uint32_t caller_sid = *(uint32_t *)((char *)caller_fp + 168);
            if (caller_sid >= caller_table->count) continue;

            XrStackMapEntry *caller_entry = &caller_table->entries[caller_sid];

            // Scan callee-saved regs from bitmap (these are in the caller's frame)
            uint32_t crbits = caller_entry->reg_bitmap;
            while (crbits) {
                int idx = __builtin_ctz(crbits);
                if (idx >= 15 && idx <= 21) {
                    int cs_off = callee_saved_offsets[idx - 15];
                    void *ptr = (void *)*(int64_t *)((char *)caller_fp + cs_off);
                    if (ptr) {
                        xr_coro_gc_markobject(gc, (XrGCHeader *)ptr);
                    }
                }
                crbits &= crbits - 1;
            }

            // Scan spill slots (PTR values written back before call)
            uint32_t csbits = caller_entry->spill_bitmap;
            while (csbits) {
                int idx = __builtin_ctz(csbits);
                void *ptr = (void *)*(int64_t *)((char *)caller_fp + 176 + idx * 8);
                if (ptr) {
                    xr_coro_gc_markobject(gc, (XrGCHeader *)ptr);
                }
                csbits &= csbits - 1;
            }
        }
    }

    // Mark external roots registered by C extensions
    for (XrCoroGCRootEntry *re = gc->root_callbacks; re; re = re->next) {
        re->callback(gc, re->userdata);
    }
}

static void markroots(XrCoroGC *gc) {
    if (!gc->owner) return;
    XR_DCHECK(gc->gcstate == XGC_PAUSE || gc->gcstate == XGC_PROPAGATE,
             "markroots: expected PAUSE or PROPAGATE state");
    // Rotate shared_refs for this new GC cycle
    gc_shared_refs_begin(gc);
    // Single bitmap: no bitmap clearing at mark start
    gc->GCmarked = 0;
    mark_coro_roots(gc);
}

/* ========== Propagate Mark ========== */

static void traverse_object(XrCoroGC *gc, XrGCHeader *obj);

static void propagatemark(XrCoroGC *gc) {
    // State can be PROPAGATE, ATOMIC, or PAUSE (youngcollection path)
    XrGCHeader *obj = xr_gclist_pop(&gc->gray);
    if (!obj) return;

    // INVARIANT 2: object was gray (on gray list), now becomes black
    if (!xr_gc_isgray(obj)) {
        fprintf(stderr, "[GC-DIAG] propagate: obj=%p type=%d marked=0x%02x objsize=%u "
                "iswhite=%d isblack=%d gcstate=%d gc_mode=%d gray.count=%d grayagain.count=%d\n",
                (void*)obj, obj->type, obj->marked, obj->objsize,
                xr_gc_iswhite(obj) ? 1 : 0, xr_gc_isblack(obj) ? 1 : 0,
                gc->gcstate, gc->gc_mode, gc->gray.count, gc->grayagain.count);
        XR_DCHECK(false, "propagate: object from gray list not gray");
    }
    xr_gc_gray2black(obj);
    XR_DCHECK(xr_gc_isblack(obj), "object not black after gray2black");
    gc->GCmarked += obj->objsize;
    gc->objects_marked++;
    traverse_object(gc, obj);
}

static void traverse_object(XrCoroGC *gc, XrGCHeader *obj) {
    xr_gc_traverse_object(gc, obj);
}

/* ========== Sweep (Per-Block) ========== */

/*
 * Sweep one Immix block: iterate local_allgc, unlink dead objects,
 * call finalizers inline, and rebuild alloc_marks in a single pass.
 * All objects in local_allgc reside in the same 16KB block → cache-friendly.
 */
static int sweep_block(XrCoroGC *gc, XrImmixBlock *block) {
    XR_DCHECK(block != NULL, "sweep_block: NULL block");
    // sweep_block called from: incremental SWEEP, entergen (PAUSE), fullgc (SWEEP),
    // youngcollection (PAUSE in GEN mode)
    // Fast path: no object in this block was marked AND no BLACK survivors exist.
    // has_marked: set during white→gray transitions (current cycle marks)
    // has_black: set by set2black (GEN mode survivors from previous minor GC cycles)
    // Both must be clear for all objects in the block to be truly dead.
    if (!block->has_marked && !block->has_black) {
        if (block->has_finalizers) {
            XrGCHeader *obj = block->local_allgc;
            while (obj) {
                XrGCHeader *next = obj->gc_next;
                if (xr_gc_needs_finalize(obj->type)) {
                    XrGCDestroyFn destroy = get_destroy_func(obj->type);
                    if (destroy) { destroy(obj, gc); gc->finalizer_count++; gc->objects_finalized++; }
                }
                obj = next;
            }
        }
        gc->totalbytes -= block->alloc_bytes;
        gc->objects_swept += block->alloc_count;
        gc->object_count -= block->alloc_count;
        block->local_allgc = NULL;
        block->alloc_count = 0;
        block->alloc_bytes = 0;
        block->has_finalizers = 0;
        block->has_black = 0;
        block->alloc_marks[0] = 1ULL;  // line 0 reserved
        block->alloc_marks[1] = 0;
        return 0;
    }
    block->has_marked = 0;  // reset for next cycle

    uint8_t deadwhite = gc->currentwhite ^ XGC_WHITEBITS;
    uint64_t new_marks[2] = {1ULL, 0};  // line 0 always reserved
    uint32_t live_count = 0;
    int64_t live_bytes = 0;
    uint8_t live_has_fin = 0;
    uint8_t live_has_black = 0;  // track BLACK survivors for next cycle's fast path
    XrGCHeader **p = &block->local_allgc;

    while (*p) {
        XrGCHeader *curr = *p;
        uint8_t marked = curr->marked;

        if ((marked & XGC_WHITEBITS) == deadwhite) {
            // Dead object: unlink from per-block list
            *p = curr->gc_next;

            if (xr_gc_needs_finalize(curr->type)) {
                XrGCDestroyFn destroy = get_destroy_func(curr->type);
                if (destroy) {
                    destroy(curr, gc);
                    gc->finalizer_count++;
                    gc->objects_finalized++;
                }
            }

            gc->totalbytes -= curr->objsize;
            gc->object_count--;
            gc->objects_swept++;
        } else {
            // Alive: reset color for next cycle.
            if (gc->gc_mode != XGC_MODE_GEN) {
                curr->marked = (marked & ~(XGC_WHITEBITS | XGC_BLACK)) | gc->currentwhite;
            } else if (xr_gc_isblack(curr)) {
                // GEN mode: track BLACK survivors so next minor GC skips fast path
                live_has_black = 1;
            }

            int first = XR_IMMIX_LINE_INDEX(curr);
            int last  = XR_IMMIX_LINE_INDEX((char*)curr + curr->objsize - 1);
            for (int l = first; l <= last; l++)
                XR_IMMIX_LINE_SET(new_marks, l);

            live_count++;
            live_bytes += curr->objsize;
            if (xr_gc_needs_finalize(curr->type))
                live_has_fin = 1;

            p = &curr->gc_next;
        }
    }

    block->alloc_count = live_count;
    block->alloc_bytes = live_bytes;
    block->has_finalizers = live_has_fin;
    block->has_black = live_has_black;
    block->alloc_marks[0] = new_marks[0];
    block->alloc_marks[1] = new_marks[1];

    uint64_t w0 = new_marks[0] & ~1ULL;
    uint64_t w1 = new_marks[1];
    return __builtin_popcountll(w0) + __builtin_popcountll(w1);
}

/*
 * Sweep all blocks in a block list. Returns number of blocks swept.
 */
static int sweep_blocklist(XrCoroGC *gc, XrImmixBlock *list) {
    XR_DCHECK(gc != NULL, "sweep_blocklist: NULL gc");
    int count = 0;
    for (XrImmixBlock *b = list; b; b = b->next) {
        sweep_block(gc, b);
        count++;
    }
    return count;
}

// Sweep large objects (single list)
static void sweeplargeobjects(XrCoroGC *gc) {
    XR_DCHECK(gc != NULL, "sweeplargeobjects: NULL gc");
    uint8_t deadwhite = gc->currentwhite ^ XGC_WHITEBITS;
    XrGCHeader **p = &gc->large_objects;

    while (*p) {
        XrGCHeader *curr = *p;
        uint8_t marked = curr->marked;

        if ((marked & XGC_WHITEBITS) == deadwhite) {
            if (xr_gc_needs_finalize(curr->type)) {
                XrGCDestroyFn destroy = get_destroy_func(curr->type);
                if (destroy) {
                    gc->finalizer_count++;
                    gc->objects_finalized++;
                    destroy(curr, gc);
                }
            }
            *p = curr->gc_next;
            gc->totalbytes -= curr->objsize;
            gc->large_bytes -= curr->objsize;
            gc->object_count--;
            gc->objects_swept++;
            if (XR_GC_IS_MMAP(curr)) {
                munmap(curr, curr->objsize);
            } else {
                xr_free(curr);
            }
        } else {
            curr->marked = (marked & ~(XGC_WHITEBITS | XGC_BLACK)) | gc->currentwhite;
            p = &curr->gc_next;
        }
    }
}

/* ========== Incremental Sweep State Machine ========== */

static void setpause(XrCoroGC *gc);  // Forward declaration (defined after gen GC section)

/*
 * Compute mark-step byte budget proportional to GCdebt.
 * Higher debt → more work per step to keep up with allocation.
 */
static inline int64_t mark_step_budget(XrCoroGC *gc) {
    int64_t debt = gc->GCdebt > 0 ? gc->GCdebt : 0;
    int64_t work = debt * gc->gc_stepmul / 100;
    if (work < XGC_MARK_STEP_MIN) work = XGC_MARK_STEP_MIN;
    if (work > XGC_MARK_STEP_MAX) work = XGC_MARK_STEP_MAX;
    return work;
}

/*
 * Compute sweep-step unit budget proportional to GCdebt.
 * 1 unit = 1 block (or 1 batch of large objects / 1 reclaim).
 */
static inline int sweep_step_budget(XrCoroGC *gc) {
    int64_t debt = gc->GCdebt > 0 ? gc->GCdebt : 0;
    // Rough heuristic: 1 block per 4KB of debt
    int units = (int)(debt / (4 * 1024)) + XGC_SWEEP_UNITS_MIN;
    if (units > XGC_SWEEP_UNITS_MAX) units = XGC_SWEEP_UNITS_MAX;
    return units;
}

// Initialize incremental sweep cursors.  Called at ATOMIC → SWEEP transition.
static void sweep_start(XrCoroGC *gc) {
    gc->sweep_phase = XGC_SWEEP_FULL_BLOCKS;
    gc->sweep_block = gc->immix.full_blocks;
}

// Shrink a gray list if capacity is much larger than recent peak usage.
// Avoids permanent high-water memory after a one-time large job.
static void maybe_shrink_graylist(XrGCGrayList *list) {
    if (list->capacity > 64 && list->capacity > list->peak * 4) {
        int newcap = list->peak * 2;
        if (newcap < 64) newcap = 64;
        XrGCHeader **p = xr_realloc(list->items, (size_t)newcap * sizeof(XrGCHeader*));
        if (p) { list->items = p; list->capacity = newcap; }
    }
    list->peak = 0;
}

// Finalize sweep cycle: decref shared objects, state transition, verify.
// Shared by incremental completion path.  STW paths (fullgc/entergen)
// handle timing themselves and call the pieces directly.
static void finalize_sweep(XrCoroGC *gc) {
    gc_shared_refs_end(gc);
    gc->gcstate = XGC_PAUSE;
    gc->gc_count++;
    setpause(gc);
    maybe_shrink_graylist(&gc->gray);
    maybe_shrink_graylist(&gc->grayagain);
    xr_gc_verify_invariants(gc);
}

/*
 * Incremental sweep: process up to `budget` units.
 * 1 unit = 1 block or 1 batch of all large objects or 1 reclaim.
 * Returns true when all sweep phases are done.
 *
 * STW fullgc does NOT use this; it sweeps everything in one go.
 */
static bool sweep_step(XrCoroGC *gc, int budget) {
    XR_DCHECK(gc->gcstate == XGC_SWEEP, "sweep_step: not in SWEEP state");
    int units = 0;

    while (units < budget) {
        switch (gc->sweep_phase) {
            case XGC_SWEEP_FULL_BLOCKS:
                if (gc->sweep_block) {
                    XrImmixBlock *next = gc->sweep_block->next;
                    sweep_block(gc, gc->sweep_block);
                    gc->sweep_block = next;
                    units++;
                } else {
                    gc->sweep_phase = XGC_SWEEP_RECYCLE_BLOCKS;
                    gc->sweep_block = gc->immix.recycle_blocks;
                }
                break;

            case XGC_SWEEP_RECYCLE_BLOCKS:
                if (gc->sweep_block) {
                    XrImmixBlock *next = gc->sweep_block->next;
                    sweep_block(gc, gc->sweep_block);
                    gc->sweep_block = next;
                    units++;
                } else {
                    gc->sweep_phase = XGC_SWEEP_CURRENT_BLOCK;
                }
                break;

            case XGC_SWEEP_CURRENT_BLOCK:
                if (gc->immix.current_block) {
                    sweep_block(gc, gc->immix.current_block);
                    units++;
                }
                gc->sweep_phase = XGC_SWEEP_LARGE_OBJECTS;
                break;

            case XGC_SWEEP_LARGE_OBJECTS:
                sweeplargeobjects(gc);
                gc->sweep_phase = XGC_SWEEP_RECLAIM;
                units++;
                break;

            case XGC_SWEEP_RECLAIM:
                xr_immix_reclaim(&gc->immix);
                gc->sweep_phase = XGC_SWEEP_DONE;
                return true;

            case XGC_SWEEP_DONE:
                return true;

            default:
                XR_DCHECK(false, "sweep_step: invalid sweep_phase");
                return true;
        }
    }
    return gc->sweep_phase == XGC_SWEEP_DONE;
}

/* ========== Atomic Phase ========== */

// Propagate all gray objects
static void propagateall(XrCoroGC *gc) {
    while (gc->gray.count > 0) {
        propagatemark(gc);
    }
}

// Clear dead keys from weak tables
static void clearweaktables(XrCoroGC *gc) {
    XR_DCHECK(gc != NULL, "clearweaktables: NULL gc");
    for (int wi = 0; wi < gc->weak.count; wi++) {
        XrGCHeader *obj = gc->weak.items[wi];

        if (obj->type == XR_TMAP) {
            // WeakMap: chained hash, scan node[] array
            struct XrMap *map = (struct XrMap*)obj;
            uint32_t size = 1u << map->lsizenode;

            for (uint32_t i = 0; i < size; i++) {
                XrMapNode *n = &map->node[i];
                if (n->key_tt != 0) {
                    XrValue key = n->key;
                    if (XR_VALUE_NEEDS_GC(key)) {
                        XrGCHeader *key_obj = XR_VALUE_GCPTR(key);
                        if (key_obj && (key_obj->marked & XGC_WHITEBITS)) {
                            n->key_tt = 0;
                            n->key = XR_NULL_VAL;
                            n->value = XR_NULL_VAL;
                            map->count--;
                        }
                    }
                }
            }
        } else if (obj->type == XR_TSET) {
            // WeakSet: open addressing, scan entries[] array
            struct XrSet *set = (struct XrSet*)obj;
            if (!set->entries) continue;

            for (uint32_t i = 0; i < set->capacity; i++) {
                XrSetEntry *e = &set->entries[i];
                if (e->state >= XR_SET_VALID) {
                    XrValue val = e->value;
                    if (XR_VALUE_NEEDS_GC(val)) {
                        XrGCHeader *val_obj = XR_VALUE_GCPTR(val);
                        if (val_obj && (val_obj->marked & XGC_WHITEBITS)) {
                            e->state = XR_SET_TOMBSTONE;
                            e->value = XR_NULL_VAL;
                            set->count--;
                        }
                    }
                }
            }
        }
    }
    xr_gclist_reset(&gc->weak);
}

// Atomic phase: non-interruptible operations
static void atomic(XrCoroGC *gc) {
    // atomic() is called from: incremental (state=ATOMIC), entergen (state=PAUSE),
    // fullgc (state=ATOMIC). Gray list may have items from entergen path.
    XR_DCHECK(gc->gcstate == XGC_ATOMIC || gc->gcstate == XGC_PAUSE,
             "atomic: unexpected state");
    // 1. Re-mark coroutine roots to catch objects created during PROPAGATE.
    //    Objects born during PROPAGATE have old currentwhite. Without this
    //    re-scan, they become deadwhite after the flip and get swept while
    //    still live on the stack. (Same as Lua's markobject(g, L) in atomic.)
    mark_coro_roots(gc);

    // 2. Process grayagain list (objects modified during mark phase)
    xr_gclist_absorb(&gc->gray, &gc->grayagain);
    propagateall(gc);

    // 3. Clear dead keys from weak tables (before flipping white)
    clearweaktables(gc);

    // 4. Flip white color (INVARIANT 3)
    uint8_t old_white = gc->currentwhite;
    gc->currentwhite ^= XGC_WHITEBITS;
    XR_DCHECK(gc->currentwhite != old_white, "white flip failed");
    XR_DCHECK((gc->currentwhite & XGC_WHITEBITS) != 0, "currentwhite has no white bits");
    (void)old_white;

    // 5. Initialize block-level sweep state
    gc->sweep_phase = 0;
    gc->sweep_block = gc->immix.full_blocks;
}

/* ========== Pause Control (Adaptive) ========== */

// Set pause time until next GC cycle
// Uses adaptive strategy based on allocation rate
static void setpause(XrCoroGC *gc) {
    XR_DCHECK(gc != NULL, "setpause: NULL gc");
    XR_DCHECK(gc->gcstate == XGC_PAUSE, "setpause: not in PAUSE state");
    // Calculate allocation rate (bytes per ms) for this cycle
    uint64_t cycle_time_ms = gc->last_gc_time_ns / 1000000;
    if (cycle_time_ms == 0) cycle_time_ms = 1;

    uint64_t alloc_rate = gc->alloc_since_gc / cycle_time_ms;
    gc->alloc_since_gc = 0;  // Reset for next cycle

    // Adaptive pause: adjust based on allocation rate
    // High allocation rate -> lower pause (more aggressive GC)
    // Low allocation rate -> higher pause (less frequent GC)
    int adaptive_pause = gc->gc_pause;

    if (alloc_rate > 10000) {
        // High pressure: >10KB/ms, reduce pause
        adaptive_pause = gc->gc_pause * 80 / 100;
        if (adaptive_pause < XGC_PAUSE_MIN) adaptive_pause = XGC_PAUSE_MIN;
    } else if (alloc_rate < 100) {
        // Low pressure: <100B/ms, increase pause
        adaptive_pause = gc->gc_pause * 150 / 100;
        if (adaptive_pause > XGC_PAUSE_MAX) adaptive_pause = XGC_PAUSE_MAX;
    }

    // Calculate threshold: marked * (pause / 100)
    int64_t threshold = gc->GCmarked * adaptive_pause / 100;

    // Debt = threshold - current, negative means "wait this much before next GC"
    int64_t debt = threshold - gc->totalbytes;
    if (debt < 0) debt = 0;
    gc->GCdebt = -debt;
}

/* ========== Sticky Immix: Minor Collection ========== */

/*
 * Sticky Immix minor collection. Non-incremental: runs to completion.
 * Only sweeps young blocks. Old blocks are untouched.
 *
 * Steps:
 *   1. Mark roots + remembered set (grayagain = old-block objects that wrote young refs)
 *   2. Propagate marks (only follows young-block objects)
 *   3. Flip white
 *   4. Sweep young blocks only
 *   5. Promote high-survival young blocks to old (no object movement)
 *   6. Reclaim empty young blocks
 *   7. Sweep large objects
 */
static void youngcollection(XrCoroGC *gc) {
    XR_DCHECK(gc != NULL, "youngcollection: NULL gc");
    XR_DCHECK(gc->gc_mode == XGC_MODE_GEN, "youngcollection: not in GEN mode");
    XR_DCHECK(gc->owner != NULL, "youngcollection: no owner coroutine");
    uint64_t t0 = xr_gc_time_ns();
    // 1. Mark roots
    gc->GCmarked = 0;
    mark_coro_roots(gc);

    // 2. Process remembered set: old-block objects that may point to young objects.
    //
    // Key insight: after propagateall, objects become BLACK. We leave them
    // BLACK. Subsequent barrierback calls on the SAME object between this
    // and the next minor GC will push it again (since isblack → true).
    // We deduplicate using REM bits:
    //   - Before traverse: tag all existing items with REM_2
    //   - After propagateall: keep items with REM_2 (unique), skip others (dupes)
    //   - New items from barrierback during propagate have REM_1
    {
        int rcount = gc->grayagain.count;
        // Tag existing items for dedup, then push to gray
        for (int i = 0; i < rcount; i++) {
            XrGCHeader *obj = gc->grayagain.items[i];
            if (xr_gc_get_rem(obj) == XGC_REM_2) {
                // Already tagged (duplicate in this batch) → skip traverse
                continue;
            }
            xr_gc_set_rem(obj, XGC_REM_2);
            if (xr_gc_isblack(obj))
                xr_gc_black2gray(obj);
            xr_gclist_push(&gc->gray, obj);
        }
        propagateall(gc);

        // Rebuild: keep unique old-block objects (REM_2), skip duplicates
        int new_count = 0;
        for (int i = 0; i < rcount; i++) {
            XrGCHeader *obj = gc->grayagain.items[i];
            if (xr_gc_get_rem(obj) != XGC_REM_2) {
                continue;  // duplicate
            }
            // Reset REM for next cycle's dedup
            xr_gc_set_rem(obj, XGC_REM_NONE);
            gc->grayagain.items[new_count++] = obj;
        }
        // Append new items added during propagate (at positions >= rcount)
        for (int i = rcount; i < gc->grayagain.count; i++) {
            XrGCHeader *obj = gc->grayagain.items[i];
            xr_gc_set_rem(obj, XGC_REM_NONE);
            gc->grayagain.items[new_count++] = obj;
        }
        gc->grayagain.count = new_count;
    }

    // 3. Clear weak tables
    clearweaktables(gc);

    // 4. Flip white
    gc->currentwhite ^= XGC_WHITEBITS;

    // 5. Sweep young blocks + classify in single pass.
    // sweep_block updates alloc_marks; use those directly to classify blocks.
    {
        XrImmixBlock *new_full    = NULL;
        XrImmixBlock *new_recycle = NULL;
        XrImmixBlock *new_free    = gc->immix.free_blocks;
        int64_t promoted_bytes = 0;

        // Helper macro: sweep block then classify based on live line count
        #define SWEEP_AND_CLASSIFY(blk) do {                                    \
            int live = sweep_block(gc, (blk));                                  \
            if (live == 0) {                                                    \
                (blk)->is_young = 1;                                            \
                (blk)->local_allgc = NULL;                                      \
                (blk)->has_black = 0;                                           \
                (blk)->next = new_free;                                         \
                new_free = (blk);                                               \
                    } else if (live * 100 / XR_IMMIX_USABLE_LINES >= XGC_PROMOTE_THRESHOLD_PCT) { \
                (blk)->is_young = 0;                                            \
                for (XrGCHeader *_o = (blk)->local_allgc; _o; _o = _o->gc_next) \
                    xr_gc_set2black(_o);                                        \
                (blk)->has_black = 1;                                           \
                (blk)->next = gc->immix.old_blocks;                             \
                gc->immix.old_blocks = (blk);                                   \
                gc->immix.old_block_count++;                                    \
                promoted_bytes += (int64_t)live * XR_IMMIX_LINE_SIZE;           \
            } else if (live < XR_IMMIX_USABLE_LINES) {                         \
                (blk)->next_scan_line = XR_IMMIX_FIRST_LINE;                    \
                (blk)->next = new_recycle;                                      \
                new_recycle = (blk);                                            \
            } else {                                                            \
                (blk)->next = new_full;                                         \
                new_full = (blk);                                               \
            }                                                                   \
        } while(0)

        // Process full_blocks and recycle_blocks
        XrImmixBlock *ylists[] = { gc->immix.full_blocks, gc->immix.recycle_blocks };
        for (int li = 0; li < 2; li++) {
            XrImmixBlock *b = ylists[li];
            while (b) {
                XrImmixBlock *next = b->next;
                SWEEP_AND_CLASSIFY(b);
                b = next;
            }
        }
        if (gc->immix.current_block) {
            SWEEP_AND_CLASSIFY(gc->immix.current_block);
            gc->immix.current_block = NULL;
        }
        #undef SWEEP_AND_CLASSIFY

        gc->immix.full_blocks = new_full;
        gc->immix.recycle_blocks = new_recycle;
        gc->immix.free_blocks = new_free;
        gc->immix.cursor = NULL;
        gc->immix.limit = NULL;

        gc->young_promoted += promoted_bytes;
    }

    // 6. Sweep large objects
    sweeplargeobjects(gc);

    // 8. Decref shared objects no longer referenced after this collection
    gc_shared_refs_end(gc);

    xr_gc_verify_invariants(gc);

    XR_DCHECK(gc->totalbytes >= 0, "youngcollection: totalbytes negative after sweep");

    // 9. Keep gcstate = PROPAGATE so write barriers stay active between minor GCs
    gc->gcstate = XGC_PROPAGATE;

    // 10. Statistics
    uint64_t elapsed = xr_gc_time_ns() - t0;
    gc->last_gc_time_ns = elapsed;
    gc->gc_time_ns += elapsed;
    gc->gc_count++;
    // alloc_since_gc reset moved to caller (xr_coro_gc_step gen-mode path)
    // so entergen can still see the accumulated allocation rate.
}

/*
 * Transition from generational to incremental mode (for major GC).
 * Merges old blocks back into young lists and resets all colors.
 */
static void minor2inc(XrCoroGC *gc) {
    XR_DCHECK(gc != NULL, "minor2inc: NULL gc");
    XR_DCHECK(gc->gc_mode == XGC_MODE_GEN, "minor2inc: not in GEN mode");
    gc->gc_mode = XGC_MODE_INC;
    gc->GCest = gc->totalbytes;

    // Merge old blocks into full_blocks (they'll be swept in the full GC)
    while (gc->immix.old_blocks) {
        XrImmixBlock *b = gc->immix.old_blocks;
        gc->immix.old_blocks = b->next;
        b->is_young = 1;  // reset for incremental mode (no young/old distinction)
        b->next = gc->immix.full_blocks;
        gc->immix.full_blocks = b;
    }
    gc->immix.old_block_count = 0;

    // Reset all object colors to current white for fresh mark-sweep
    uint8_t newcolor = gc->currentwhite & XGC_WHITEBITS;
    XrImmixBlock *lists[] = { gc->immix.full_blocks, gc->immix.recycle_blocks, gc->immix.current_block };
    for (int i = 0; i < 3; i++) {
        for (XrImmixBlock *b = lists[i]; b; b = b->next) {
            if (!b->local_allgc) continue;  // skip empty blocks
            for (XrGCHeader *obj = b->local_allgc; obj; obj = obj->gc_next) {
                obj->marked = (obj->marked & ~XGC_GCBITS) | newcolor;
            }
        }
        if (i == 2) break;
    }
    for (XrGCHeader *obj = gc->large_objects; obj; obj = obj->gc_next) {
        obj->marked = (obj->marked & ~XGC_GCBITS) | (gc->currentwhite & XGC_WHITEBITS);
    }
    xr_gclist_reset(&gc->grayagain);
}

/*
 * After a full incremental cycle, transition all surviving objects to old
 * and switch back to generational mode (Sticky Immix).
 * All surviving blocks become old blocks; new allocations go to fresh young blocks.
 */
static void atomic2gen(XrCoroGC *gc) {
    XR_DCHECK(gc != NULL, "atomic2gen: NULL gc");
    XR_DCHECK(gc->gc_mode == XGC_MODE_INC, "atomic2gen: not in INC mode");
    XR_DCHECK(gc->gcstate == XGC_PAUSE, "atomic2gen: expected PAUSE state");
    gc->gc_mode = XGC_MODE_GEN;
    gc->young_promoted = 0;
    gc->GCest = gc->totalbytes;

    // Move all surviving blocks to old_blocks
    XrImmixBlock *blists[] = { gc->immix.full_blocks, gc->immix.recycle_blocks, gc->immix.current_block };
    for (int i = 0; i < 3; i++) {
        XrImmixBlock *b = blists[i];
        while (b) {
            XrImmixBlock *next = b->next;
            if (b->alloc_count > 0) {
                b->is_young = 0;
                for (XrGCHeader *obj = b->local_allgc; obj; obj = obj->gc_next)
                    xr_gc_set2black(obj);
                b->has_black = 1;
                b->next = gc->immix.old_blocks;
                gc->immix.old_blocks = b;
                gc->immix.old_block_count++;
            } else {
                // Empty block: return to free pool
                b->is_young = 1;
                b->local_allgc = NULL;
                b->next = gc->immix.free_blocks;
                gc->immix.free_blocks = b;
            }
            b = next;
            if (i == 2) break;  // current_block is single
        }
    }
    gc->immix.full_blocks = NULL;
    gc->immix.recycle_blocks = NULL;
    gc->immix.current_block = NULL;
    gc->immix.cursor = NULL;
    gc->immix.limit = NULL;

    // Set large objects to black
    for (XrGCHeader *obj = gc->large_objects; obj; obj = obj->gc_next)
        xr_gc_set2black(obj);

    // Recount total blocks
    {
        size_t old_cnt = 0, free_cnt = 0;
        for (XrImmixBlock *p = gc->immix.old_blocks; p; p = p->next) old_cnt++;
        for (XrImmixBlock *p = gc->immix.free_blocks; p; p = p->next) free_cnt++;
        gc->immix.old_block_count = old_cnt;
        gc->immix.total_blocks = old_cnt + free_cnt;
        gc->immix.total_block_bytes = gc->immix.total_blocks * XR_IMMIX_BLOCK_SIZE;
    }

    xr_gclist_reset(&gc->grayagain);
}

/*
 * Enter generational mode. Run a full incremental cycle first to
 * ensure all objects are correctly marked, then transition to gen.
 */
static void entergen(XrCoroGC *gc) {
    XR_DCHECK(gc != NULL, "entergen: NULL gc");
    gc->gc_mode = XGC_MODE_INC;
    gc->gcstate = XGC_PAUSE;
    xr_gclist_reset(&gc->gray);
    xr_gclist_reset(&gc->grayagain);

    markroots(gc);
    gc->gcstate = XGC_PROPAGATE;
    propagateall(gc);
    XR_DCHECK(gc->gray.count == 0, "entergen: gray list not empty after propagateall");
    gc->gcstate = XGC_ATOMIC;
    atomic(gc);

    // Sweep everything
    sweep_blocklist(gc, gc->immix.full_blocks);
    sweep_blocklist(gc, gc->immix.recycle_blocks);
    if (gc->immix.current_block)
        sweep_block(gc, gc->immix.current_block);
    sweeplargeobjects(gc);
    xr_immix_reclaim(&gc->immix);

    // Complete sweep → PAUSE before transitioning to generational mode
    gc->gcstate = XGC_PAUSE;

    // Transition all surviving objects/blocks to old
    atomic2gen(gc);
    gc->gcstate = XGC_PROPAGATE;
}

/*
 * Set debt for next minor collection.
 * Minor GC triggers when allocated bytes reach ~50% of estimated live bytes.
 */
static void setminordebt(XrCoroGC *gc) {
    int64_t estimate = gc->GCest > 0 ? gc->GCest : gc->totalbytes;
    int64_t threshold = estimate * 50 / 100;
    if (threshold < 4096) threshold = 4096;
    gc->GCdebt = -threshold;
}

/*
 * Check whether to shift from minor to major collection.
 * Triggers major if promoted bytes exceed XGC_MAJOR_TRIGGER_PCT% of
 * estimated live bytes.  Lower threshold (150 vs old 400) catches
 * old-object accumulation earlier, reducing peak RSS.
 */
static bool check_minor_to_major(XrCoroGC *gc) {
    if (gc->GCest <= 0) return false;
    int64_t limit = gc->GCest * XGC_MAJOR_TRIGGER_PCT / 100;
    return gc->young_promoted >= limit;
}

/* ========== GC Step (Incremental, 4-State) ========== */

void xr_coro_gc_step(XrCoroGC *gc) {
    if (!gc || gc->in_gc || gc->gc_disabled > 0) return;

    gc->in_gc = 1;
    XR_DCHECK(gc->gcstate <= XGC_SWEEP, "gc_step entry: invalid GC state");

    // Generational mode: run minor collection (non-incremental)
    if (gc->gc_mode == XGC_MODE_GEN) {
        // First GC in gen mode: must run entergen to do initial full mark-sweep
        // and classify all existing objects as OLD before minor collections.
        if (gc->GCest == 0) {
            entergen(gc);
            setminordebt(gc);
            gc->in_gc = 0;
            return;
        }

        youngcollection(gc);

        if (check_minor_to_major(gc)) {
            minor2inc(gc);
            entergen(gc);
            setminordebt(gc);
        } else {
            setminordebt(gc);
        }

        gc->alloc_since_gc = 0;  // Reset once after all gen-mode work
        gc->in_gc = 0;
        return;
    }

    // Incremental mode (also used as major GC)
    XR_DCHECK(gc->gcstate <= XGC_SWEEP, "gc_step: invalid GC state");
    switch (gc->gcstate) {
        case XGC_PAUSE:
            gc->gc_cycle_start_ns = xr_gc_time_ns();
            // Reset per-cycle stats
            gc->objects_marked = 0;
            gc->objects_swept = 0;
            gc->objects_finalized = 0;
            gc->objects_promoted = 0;
            gc->mark_time_ns = 0;
            gc->sweep_time_ns = 0;
            gc->mark_start_ns = gc->gc_cycle_start_ns;
            markroots(gc);
            gc->gcstate = XGC_PROPAGATE;
            break;

        case XGC_PROPAGATE: {
            if (gc->gray.count > 0) {
                int64_t budget = mark_step_budget(gc);
                int64_t scanned = 0;
                while (gc->gray.count > 0 && scanned < budget) {
                    scanned += gc->gray.items[gc->gray.count - 1]->objsize;
                    propagatemark(gc);
                }
            }
            if (gc->gray.count == 0) {
                gc->gcstate = XGC_ATOMIC;
            }
            break;
        }

        case XGC_ATOMIC: {
            atomic(gc);
            gc->mark_time_ns = xr_gc_time_ns() - gc->mark_start_ns;
            gc->gcstate = XGC_SWEEP;
            sweep_start(gc);
            break;
        }

        case XGC_SWEEP: {
            if (sweep_step(gc, sweep_step_budget(gc))) {
                // All sweep phases done — record timing and finalize
                uint64_t now = xr_gc_time_ns();
                gc->sweep_time_ns = now - (gc->gc_cycle_start_ns + gc->mark_time_ns);
                uint64_t elapsed = now - gc->gc_cycle_start_ns;
                gc->last_gc_time_ns = elapsed;
                gc->gc_time_ns += elapsed;
                finalize_sweep(gc);
            }
            break;
        }
    }

    gc->in_gc = 0;
}

/* ========== Full GC ========== */

void xr_coro_gc_fullgc(XrCoroGC *gc) {
    if (!gc) return;
    XR_DCHECK(!gc->in_gc, "fullgc: re-entry during GC");

    uint64_t t0 = xr_gc_time_ns();
    uint8_t saved_mode = gc->gc_mode;
    gc->gc_disabled++;

    // Switch to incremental mode for full collection
    if (gc->gc_mode == XGC_MODE_GEN) {
        minor2inc(gc);
        XR_DCHECK(gc->gc_mode == XGC_MODE_INC, "fullgc: minor2inc didn't switch to INC");
    }

    // Abort any pending cycle (we're about to do a full fresh cycle)
    gc->gcstate = XGC_PAUSE;
    xr_gclist_reset(&gc->gray);
    xr_gclist_reset(&gc->grayagain);


    // PAUSE -> PROPAGATE: mark roots
    markroots(gc);
    gc->gcstate = XGC_PROPAGATE;

    // PROPAGATE: mark all reachable objects
    while (gc->gray.count > 0) {
        propagatemark(gc);
    }
    XR_DCHECK(gc->gray.count == 0, "fullgc: gray list not empty after propagateall");

    // ATOMIC: process grayagain, flip white
    gc->gcstate = XGC_ATOMIC;
    atomic(gc);

    // SWEEP: sweep all blocks (old blocks were merged by minor2inc)
    gc->gcstate = XGC_SWEEP;
    sweep_blocklist(gc, gc->immix.full_blocks);
    sweep_blocklist(gc, gc->immix.recycle_blocks);
    if (gc->immix.current_block) {
        sweep_block(gc, gc->immix.current_block);
    }
    sweeplargeobjects(gc);
    xr_immix_reclaim(&gc->immix);

    // Decref shared objects no longer referenced
    gc_shared_refs_end(gc);

    // Record cycle timing
    uint64_t elapsed = xr_gc_time_ns() - t0;
    gc->last_gc_time_ns = elapsed;
    gc->gc_time_ns += elapsed;
    gc->gcstate = XGC_PAUSE;
    gc->gc_count++;
    XR_DCHECK(gc->totalbytes >= 0, "fullgc: totalbytes negative after sweep");

    // Return to generational mode if that was the original mode
    if (saved_mode == XGC_MODE_GEN) {
        atomic2gen(gc);
        setminordebt(gc);
    } else {
        setpause(gc);
    }

    xr_gc_verify_invariants(gc);

    // NOTE: global string pool sweep removed from per-coroutine fullgc.
    // xr_global_pool_sweep is a global operation but ACCESSED flags are only
    // set by the current coroutine's mark phase. Other coroutines' live strings
    // would be incorrectly freed. Pool cleanup happens at isolate shutdown.

    gc->gc_disabled--;
}

/* ========== External Root Registration ========== */

int xr_coro_gc_register_root(XrCoroGC *gc, XrCoroGCRootCallback callback, void *userdata) {
    if (!gc || !callback) return -1;

    XrCoroGCRootEntry *entry = (XrCoroGCRootEntry*)xr_malloc(sizeof(XrCoroGCRootEntry));
    if (!entry) return -1;

    entry->callback = callback;
    entry->userdata = userdata;
    entry->next = gc->root_callbacks;
    gc->root_callbacks = entry;
    return 0;
}

int xr_coro_gc_unregister_root(XrCoroGC *gc, XrCoroGCRootCallback callback, void *userdata) {
    if (!gc || !callback) return -1;

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

/* ========== Debug Invariant Verification ========== */

#if XR_GC_DEBUG
/*
 * Verify GC invariants after critical operations.
 * Catches all 4 historical bug patterns:
 *   INV-1: Immix alloc_marks match actual objects (Bug #1)
 *   INV-2: gray list objects are actually gray (Bug #3)
 *   INV-3: large objects never in Immix blocks (Bug P0-2)
 *   INV-4: block->alloc_count matches local_allgc chain (Bug P0-3)
 *   INV-5: no black object has deadwhite children (tri-color)
 *
 * Only compiled when XR_GC_DEBUG=1. Cost: O(n) per call.
 */
void xr_gc_verify_invariants(XrCoroGC *gc) {
    if (!gc) return;
    int errors = 0;

    // INV-1 + INV-4: walk all Immix blocks, verify alloc_count and alloc_marks
    XrImmixBlock *blists[] = {
        gc->immix.full_blocks,
        gc->immix.recycle_blocks,
        gc->immix.current_block,
        gc->immix.old_blocks
    };
    for (int li = 0; li < 4; li++) {
        for (XrImmixBlock *b = blists[li]; b; b = b->next) {
            uint32_t counted = 0;
            for (XrGCHeader *obj = b->local_allgc; obj; obj = obj->gc_next) {
                counted++;
                // INV-1: every live object's lines must be set in alloc_marks
                int first = XR_IMMIX_LINE_INDEX(obj);
                if (!XR_IMMIX_LINE_GET(b->alloc_marks, first)) {
                    fprintf(stderr, "[GC-INV] INV-1 FAIL: obj %p type=%d line %d not in alloc_marks (block %p)\n",
                            (void*)obj, obj->type, first, (void*)b);
                    errors++;
                }
                // INV-3: Immix object size must be <= large threshold
                if (obj->objsize > XR_LARGE_OBJECT_THRESHOLD) {
                    fprintf(stderr, "[GC-INV] INV-3 FAIL: oversized obj %p size=%u in Immix block %p\n",
                            (void*)obj, obj->objsize, (void*)b);
                    errors++;
                }
            }
            // INV-4: counted must match block->alloc_count
            if (counted != b->alloc_count) {
                fprintf(stderr, "[GC-INV] INV-4 FAIL: block %p alloc_count=%u but chain has %u objects\n",
                        (void*)b, b->alloc_count, counted);
                errors++;
            }
            if (li == 2) break;  // current_block is single
        }
    }

    // INV-2: all gray list objects must be gray (not white, not black)
    for (int i = 0; i < gc->gray.count; i++) {
        XrGCHeader *obj = gc->gray.items[i];
        if (!xr_gc_isgray(obj)) {
            fprintf(stderr, "[GC-INV] INV-2 FAIL: gray list item %p marked=0x%02x is NOT gray\n",
                    (void*)obj, obj->marked);
            errors++;
        }
    }

    // INV-3 (large objects): verify they are NOT inside any Immix block
    // (heuristic: check alignment — Immix blocks are 16KB-aligned)
    for (XrGCHeader *obj = gc->large_objects; obj; obj = obj->gc_next) {
        if (obj->objsize <= XR_LARGE_OBJECT_THRESHOLD) {
            fprintf(stderr, "[GC-INV] INV-3 FAIL: small obj %p size=%u in large_objects list\n",
                    (void*)obj, obj->objsize);
            errors++;
        }
    }

    // INV-6: totalbytes == Σ block.alloc_bytes + large_bytes
    {
        int64_t computed = gc->large_bytes;
        for (int li = 0; li < 4; li++) {
            for (XrImmixBlock *b = blists[li]; b; b = b->next) {
                computed += b->alloc_bytes;
                if (li == 2) break;
            }
        }
        if (computed != gc->totalbytes) {
            fprintf(stderr, "[GC-INV] INV-6 FAIL: totalbytes=%lld but computed=%lld (large_bytes=%lld)\n",
                    (long long)gc->totalbytes, (long long)computed, (long long)gc->large_bytes);
            errors++;
        }
    }

    // INV-7: object_count == Σ block.alloc_count + count(large_objects)
    {
        uint32_t computed = 0;
        for (int li = 0; li < 4; li++) {
            for (XrImmixBlock *b = blists[li]; b; b = b->next) {
                computed += b->alloc_count;
                if (li == 2) break;
            }
        }
        uint32_t large_count = 0;
        for (XrGCHeader *obj = gc->large_objects; obj; obj = obj->gc_next)
            large_count++;
        computed += large_count;
        if (computed != gc->object_count) {
            fprintf(stderr, "[GC-INV] INV-7 FAIL: object_count=%u but computed=%u (large=%u)\n",
                    gc->object_count, computed, large_count);
            errors++;
        }
    }

    if (errors > 0) {
        fprintf(stderr, "[GC-INV] %d invariant violations detected (gc_count=%u state=%s)\n",
                errors, gc->gc_count, xr_gc_state_name(gc->gcstate));
    }
}
#endif // XR_GC_DEBUG

/* ========== Debug ========== */

void xr_coro_gc_print_stats(XrCoroGC *gc) {
    if (!gc) {
        printf("XrCoroGC: NULL\n");
        return;
    }

    printf("=== XrCoroGC (Incremental Immix) ===\n");
    printf("GC state:     %s\n", xr_gc_state_name(gc->gcstate));
    printf("Total bytes:  %lld\n", (long long)gc->totalbytes);
    printf("GC debt:      %lld\n", (long long)gc->GCdebt);
    printf("GC marked:    %lld\n", (long long)gc->GCmarked);
    printf("GC count:     %u\n", gc->gc_count);
    printf("GC pause:     %d%%\n", gc->gc_pause);
    printf("GC stepmul:   %d\n", gc->gc_stepmul);
    printf("Current white: %s\n", gc->currentwhite == XGC_WHITE0 ? "WHITE0" : "WHITE1");

    XrImmixStats istats;
    xr_immix_get_stats(&gc->immix, &istats);
    printf("Immix blocks:   %zu (full=%zu recycle=%zu free=%zu)\n",
           istats.total_blocks, istats.full_blocks,
           istats.recycle_blocks, istats.free_blocks);
    printf("Immix memory:   %zu bytes\n", istats.total_bytes);
    printf("Immix lines:    live=%zu free=%zu\n",
           istats.live_lines, istats.free_lines);

    size_t obj_count = 0;
    XrImmixBlock *plists[] = {
        gc->immix.full_blocks,
        gc->immix.recycle_blocks,
        gc->immix.current_block
    };
    for (int i = 0; i < 3; i++) {
        for (XrImmixBlock *b = plists[i]; b; b = b->next) {
            for (XrGCHeader *o = b->local_allgc; o; o = o->gc_next)
                obj_count++;
        }
        if (i == 2) break;
    }
    printf("Object count:   %zu\n", obj_count);

    size_t large_count = 0;
    for (XrGCHeader *obj = gc->large_objects; obj; obj = obj->gc_next)
        large_count++;
    printf("Large objects:  %zu (%lld bytes)\n", large_count, (long long)gc->large_bytes);

    // Timing
    printf("GC time total:  %llu us\n", (unsigned long long)(gc->gc_time_ns / 1000));
    printf("Last cycle:     %llu us (mark=%llu sweep=%llu)\n",
           (unsigned long long)(gc->last_gc_time_ns / 1000),
           (unsigned long long)(gc->mark_time_ns / 1000),
           (unsigned long long)(gc->sweep_time_ns / 1000));

    // Per-cycle counters (from last completed cycle)
    printf("Last cycle objects: marked=%u swept=%u finalized=%u promoted=%u\n",
           gc->objects_marked, gc->objects_swept,
           gc->objects_finalized, gc->objects_promoted);
    printf("Finalizers total: %u\n", gc->finalizer_count);

    printf("=====================================\n");
}

/* ========== JIT Allocation Helper ========== */

// Slow path: full allocation when inline bump fails
// CALL_C convention: (coro, packed_arg)
// packed_arg = (uint64_t)gc_type << 32 | aligned_size
XrGCHeader* xr_jit_alloc(struct XrCoroutine *coro, uint64_t type_and_size) {
    uint8_t type = (uint8_t)(type_and_size >> 32);
    uint32_t size = (uint32_t)(type_and_size & 0xFFFFFFFF);
    if (!coro || !coro->coro_gc) return NULL;
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
    (void)coro;
    void *p = (void *)(uintptr_t)obj_ptr;
    if (!p) return;
    XrGCHeader *obj = (XrGCHeader *)p;
    xr_immix_mark_alloc_lines_fast(obj, obj->objsize);
}

// Fast path post-alloc: GC bookkeeping after inline bump succeeds
// GC header already initialized by JIT code. This handles:
//   1. alloc_marks (line occupancy bitmap)
//   2. allgc link (per-block object list for sweep)
//   3. stats update (totalbytes, object_count, GCdebt)
// CALL_C convention: (coro, obj_ptr)
void xr_jit_alloc_post(struct XrCoroutine *coro, void *obj_ptr) {
    if (!coro || !coro->coro_gc || !obj_ptr) return;
    XrCoroGC *gc = coro->coro_gc;
    XrGCHeader *obj = (XrGCHeader *)obj_ptr;
    uint32_t total = obj->objsize;
    XR_DCHECK(total > 0, "jit_alloc_post: zero objsize");
    XR_DCHECK(total <= XR_LARGE_OBJECT_THRESHOLD, "jit_alloc_post: oversized for Immix");

    gc_post_immix_alloc(obj, obj->type, total);
    gc_update_alloc_stats(gc, total);
}

