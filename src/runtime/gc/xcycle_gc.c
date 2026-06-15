/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcycle_gc.c - Bacon-Rajan trial deletion cycle collector
 *
 * Pure reference counting cannot reclaim cyclic garbage. This collector
 * supplements the RC system by detecting and freeing dead cycles among
 * objects marked XR_OBJ_CYCLE_CANDIDATE (types whose compile-time
 * reference graph contains a cycle).
 *
 * Algorithm (Bacon & Rajan 2001, simplified):
 *   1. Potential roots: objects whose RC decremented but stayed alive.
 *   2. markGray: trial-decrement all children from each root.
 *   3. scan: if root RC >= 0 after trial decrement, it is live (0-based RC,
 *      so >= 0 means an external reference survives; scanBlack restores
 *      children). Otherwise mark white (dead cycle member).
 *   4. collect: gather the white set, RESTORE every white object's
 *      out-edge decrements, then run the normal RC destroy cascade on
 *      each white object. Restoring first means the type destructors
 *      (which drop child references and free side buffers) leave every
 *      edge dropped exactly once — live children referenced from dead
 *      cycles keep an exact refcount, and owned C resources are freed.
 *
 * Colors live in extra bits 8-9 (XR_OBJ_CYCLE_COLOR_MASK), NOT in _rsv:
 * _rsv must keep its root-index/XR_CYCLE_NOT_IN_ROOTS invariant at all
 * times or add_root/remove_root corrupt the roots array after a collect.
 *
 * Trial deletion only walks coro-local RC edges: shared / atomic /
 * runtime-managed / region children are skipped entirely (their refcount
 * is atomic or meaningless, and they cannot form coro-local cycles).
 *
 * Triggered by gc.collect() on the main coroutine (short-lived coroutines
 * are bulk-freed at termination, so cycles cannot leak from them).
 */

#include "xcoro_gc.h"
#include "xgc_header.h"
#include "xgc_internal.h"
#include "../class/xinstance.h"
#include "../class/xclass.h"
#include "../object/xarray.h"
#include "../object/xmap.h"
#include "../object/xset.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"

#include <string.h>

/* Initial capacity for cycle_roots (lazy; NULL until first add). */
#define XR_CYCLE_ROOTS_INIT_CAP 64

/* Color encoding for trial deletion (extra bits 8-9, see xgc_header.h).
 * Every reachable object is recolored before its color is read in each
 * phase, so stale colors from a previous collect round are harmless. */
#define COLOR_BLACK 0 /* definitely live */
#define COLOR_GRAY 1  /* in the reachable set / trial-decremented */
#define COLOR_WHITE 2 /* presumed dead (cycle garbage) */

static inline void set_color(XrGCHeader *obj, uint8_t color) {
    obj->extra = (uint16_t) ((obj->extra & ~(uint16_t) XR_OBJ_CYCLE_COLOR_MASK) |
                             ((uint16_t) color << XR_OBJ_CYCLE_COLOR_SHIFT));
}

static inline uint8_t get_color(const XrGCHeader *obj) {
    return (uint8_t) ((obj->extra & XR_OBJ_CYCLE_COLOR_MASK) >> XR_OBJ_CYCLE_COLOR_SHIFT);
}

/* Trial deletion may only touch coro-local RC objects. Shared objects use
 * atomic refcounts (raw ++/-- would race other workers), managed objects
 * are runtime-owned, and region objects ignore RC entirely. None of them
 * can be a member of a coro-local cycle, so skip the edge altogether
 * (consistently in EVERY phase, or the trial bookkeeping breaks). */
static inline bool cycle_child_eligible(const XrGCHeader *obj) {
    if (obj->extra & (XR_OBJ_REGION | XR_OBJ_MANAGED | XR_OBJ_ATOMIC))
        return false;
    if (XR_GC_IS_SHARED(obj))
        return false;
    return true;
}

/* ========== Child Scanning ========== */

/* Callback type for iterating GC-managed children of an object. */
typedef void (*ChildVisitor)(XrGCHeader *child, void *ctx);

/* Visit all GC pointer children of an object (type-specific traversal). */
static void visit_children(XrGCHeader *obj, ChildVisitor visitor, void *ctx) {
    XR_DCHECK(obj != NULL, "visit_children: NULL obj");
    switch (obj->type) {
        case XR_TINSTANCE: {
            XrInstance *inst = (XrInstance *) obj;
            XrClass *klass = inst->klass;
            if (!klass)
                break;
            uint32_t fc = xr_class_instance_field_count(klass);
            for (uint32_t i = 0; i < fc; i++) {
                XrValue v = inst->fields[i];
                if (XR_IS_PTR(v)) {
                    XrGCHeader *child = XR_VALUE_GCPTR(v);
                    if (child)
                        visitor(child, ctx);
                }
            }
            break;
        }
        case XR_TARRAY: {
            XrArray *arr = (XrArray *) obj;
            if (arr->elem_type != XR_ELEM_ANY || arr->length <= 0)
                break;
            XrValue *data = (XrValue *) arr->data;
            for (int32_t i = 0; i < arr->length; i++) {
                if (XR_IS_PTR(data[i])) {
                    XrGCHeader *child = XR_VALUE_GCPTR(data[i]);
                    if (child)
                        visitor(child, ctx);
                }
            }
            break;
        }
        case XR_TMAP: {
            XrMap *map = (XrMap *) obj;
            if (xr_map_isdummy(map) || !map->entries)
                break;
            uint32_t count = map->nentries;
            for (uint32_t i = 0; i < count; i++) {
                XrMapEntry *node = &map->entries[i];
                if (XR_MAP_ENTRY_EMPTY(node))
                    continue;
                if (XR_IS_PTR(node->key)) {
                    XrGCHeader *child = XR_VALUE_GCPTR(node->key);
                    if (child)
                        visitor(child, ctx);
                }
                if (XR_IS_PTR(node->value)) {
                    XrGCHeader *child = XR_VALUE_GCPTR(node->value);
                    if (child)
                        visitor(child, ctx);
                }
            }
            break;
        }
        case XR_TSET: {
            XrSet *set = (XrSet *) obj;
            if (!set->entries)
                break;
            for (uint32_t i = 0; i < set->capacity; i++) {
                XrSetEntry *e = &set->entries[i];
                if (e->state != XR_SET_VALID)
                    continue;
                if (XR_IS_PTR(e->value)) {
                    XrGCHeader *child = XR_VALUE_GCPTR(e->value);
                    if (child)
                        visitor(child, ctx);
                }
            }
            break;
        }
        default:
            /* Known limitation: closure/upvalue-cell edges (XR_TFUNCTION /
             * XR_TCELL) are not traversed, so cycles closed exclusively
             * through captured closures stay uncollected (a leak, never a
             * corruption — untraversed edges count as external refs and
             * keep the cycle alive). This is correct ONLY while closure
             * captures are uncounted borrows (the creating scope owns the
             * cell): traversing/trial-decrementing an uncounted edge would
             * corrupt refcounts. Enabling closure cycle collection requires
             * the compiler to first make captures owned (counted) references
             * with a balanced release on closure/cell destroy. */
            break;
    }
}

/* ========== Cycle Roots Management ========== */

XR_FUNC void xr_cycle_add_root(XrCoroGC *gc, XrGCHeader *obj) {
    if (!gc || !obj)
        return;
    if (!(obj->extra & XR_OBJ_CYCLE_CANDIDATE))
        return;
    /* Already in the set? */
    if (obj->_rsv != XR_CYCLE_NOT_IN_ROOTS)
        return;

    /* Lazy allocation. */
    if (!gc->cycle_roots) {
        gc->cycle_roots = (XrGCHeader **) xr_malloc(sizeof(XrGCHeader *) * XR_CYCLE_ROOTS_INIT_CAP);
        if (!gc->cycle_roots)
            return; /* OOM: skip cycle tracking (safe, just leaks) */
        gc->cycle_root_cap = XR_CYCLE_ROOTS_INIT_CAP;
        gc->cycle_root_count = 0;
    }

    /* Grow if needed. */
    if (gc->cycle_root_count >= gc->cycle_root_cap) {
        uint32_t new_cap = gc->cycle_root_cap * 2;
        XrGCHeader **tmp =
            (XrGCHeader **) xr_realloc(gc->cycle_roots, sizeof(XrGCHeader *) * new_cap);
        if (!tmp)
            return; /* OOM: skip */
        gc->cycle_roots = tmp;
        gc->cycle_root_cap = new_cap;
    }

    uint32_t idx = gc->cycle_root_count++;
    gc->cycle_roots[idx] = obj;
    obj->_rsv = idx;

    /* Auto-trigger: once the root set reaches the adaptive threshold, run the
     * collector. Guarded against re-entry — the destroy cascade inside fullgc
     * releases child references, which can call back here; cycle_collecting
     * suppresses a nested collection (and the threshold check) until the
     * current one finishes. */
    if (!gc->cycle_collecting && !gc->gc_disabled &&
        gc->cycle_root_count >= gc->cycle_collect_threshold)
        xr_coro_gc_fullgc(gc);
}

XR_FUNC void xr_cycle_remove_root(XrCoroGC *gc, XrGCHeader *obj) {
    if (!gc || !obj)
        return;
    uint32_t idx = obj->_rsv;
    if (idx == XR_CYCLE_NOT_IN_ROOTS || idx >= gc->cycle_root_count)
        return;

    /* Swap with last for O(1) removal. */
    uint32_t last = gc->cycle_root_count - 1;
    if (idx != last) {
        XrGCHeader *moved = gc->cycle_roots[last];
        gc->cycle_roots[idx] = moved;
        moved->_rsv = idx;
    }
    gc->cycle_root_count--;
    obj->_rsv = XR_CYCLE_NOT_IN_ROOTS;
}

XR_FUNC void xr_cycle_roots_destroy(XrCoroGC *gc) {
    if (!gc || !gc->cycle_roots)
        return;
    xr_free(gc->cycle_roots);
    gc->cycle_roots = NULL;
    gc->cycle_root_count = 0;
    gc->cycle_root_cap = 0;
}

/* ========== Bacon-Rajan Trial Deletion (iterative, OOM-safe) ==========
 *
 * Rewritten as breadth-first passes over an explicit work array so a deep
 * object graph cannot overflow the C stack (the previous version recursed
 * once per GC edge). Every allocation happens UP FRONT, before any refcount
 * is mutated: on allocation failure the collection is skipped cleanly, so a
 * trial decrement is never left unbalanced (no recursive abort path needed).
 *
 * Passes:
 *   A. BFS the eligible subgraph reachable from the roots into `R`, coloring
 *      each member GRAY (GRAY doubles as the "in R" marker). No mutation.
 *   B. markGray: decrement every internal edge exactly once (each R node's
 *      eligible children) — R is closed under eligible edges, so this is the
 *      trial deletion of all in-cycle references.
 *   C. scanBlack every still-GRAY node with refcount >= 0 (an external
 *      reference survived): restore its reachable subtree's edges and color
 *      it BLACK (live). Any node left GRAY afterwards is dead → WHITE.
 *   D. Restore the WHITE nodes' out-edges, then run the RC destroy cascade.
 */

typedef struct {
    XrGCHeader **items;
    uint32_t count;
    uint32_t cap;
    bool oom;
} CycleVec;

static bool cvec_push(CycleVec *v, XrGCHeader *o) {
    if (v->count == v->cap) {
        uint32_t nc = v->cap ? v->cap * 2 : 64;
        XrGCHeader **t = (XrGCHeader **) xr_realloc(v->items, (size_t) nc * sizeof(*t));
        if (!t) {
            v->oom = true;
            return false;
        }
        v->items = t;
        v->cap = nc;
    }
    v->items[v->count++] = o;
    return true;
}

/* Pass A visitor: add an eligible, not-yet-seen child to the reachable set. */
static void cycle_reach_visitor(XrGCHeader *child, void *ctx) {
    CycleVec *R = (CycleVec *) ctx;
    if (!child || (child->extra & XR_OBJ_DEAD) || !cycle_child_eligible(child))
        return;
    if (get_color(child) != COLOR_GRAY) {
        set_color(child, COLOR_GRAY);
        cvec_push(R, child); /* sets R->oom on failure */
    }
}

/* Pass B visitor: trial-decrement one internal edge. */
static void mark_gray_dec_visitor(XrGCHeader *child, void *ctx) {
    (void) ctx;
    if (!child || (child->extra & XR_OBJ_DEAD) || !cycle_child_eligible(child))
        return;
    child->refcount--;
}

/* Pass C visitor: restore one edge of a live node and queue newly-black
 * children. `work` is pre-sized to |R| and every pushed node is a distinct
 * member of R, so the push never exceeds capacity. */
static void scan_black_inc_visitor(XrGCHeader *child, void *ctx) {
    CycleVec *work = (CycleVec *) ctx;
    if (!child || (child->extra & XR_OBJ_DEAD) || !cycle_child_eligible(child))
        return;
    child->refcount++;
    if (get_color(child) != COLOR_BLACK) {
        set_color(child, COLOR_BLACK);
        work->items[work->count++] = child;
    }
}

static void scan_black_flat(XrGCHeader *root, CycleVec *work) {
    set_color(root, COLOR_BLACK);
    work->count = 0;
    work->items[work->count++] = root;
    while (work->count) {
        XrGCHeader *obj = work->items[--work->count];
        visit_children(obj, scan_black_inc_visitor, work);
    }
}

/* Restore one out-edge of a white object (undo its markGray decrement).
 * Edges to ineligible children were never decremented, so skip them. */
static void restore_edge_visitor(XrGCHeader *child, void *ctx) {
    (void) ctx;
    if (!child || (child->extra & XR_OBJ_DEAD) || !cycle_child_eligible(child))
        return;
    child->refcount++;
}

/* Main entry: run the Bacon-Rajan cycle collector on accumulated roots.
 * Called by gc.collect(). Runs unconditionally when there are roots. */
XR_FUNC void xr_coro_gc_fullgc(XrCoroGC *gc) {
    if (!gc)
        return;
    /* Re-entry guard: a destroy cascade started below can call add_root,
     * whose auto-trigger would otherwise recurse into the collector. */
    if (gc->cycle_collecting)
        return;
    if (!gc->cycle_roots || gc->cycle_root_count == 0) {
        /* No potential cycle roots, but a gc.collect() should still return
         * fully-dead blocks: pure-RC programs free without forming cycles. */
        xr_coro_gc_reclaim_blocks(gc);
        return;
    }

    gc->cycle_collecting = 1;
    gc->gc_count++;

    uint32_t n = gc->cycle_root_count;
    CycleVec R = {0};
    CycleVec work = {0};
    uint32_t freed = 0;

    /* Pass A: collect the reachable eligible set (colors members GRAY). */
    bool ok = true;
    for (uint32_t i = 0; i < n; i++) {
        XrGCHeader *o = gc->cycle_roots[i];
        if (o && !(o->extra & XR_OBJ_DEAD) && cycle_child_eligible(o) &&
            get_color(o) != COLOR_GRAY) {
            set_color(o, COLOR_GRAY);
            if (!cvec_push(&R, o)) {
                ok = false;
                break;
            }
        }
    }
    for (uint32_t s = 0; ok && s < R.count; s++) {
        visit_children(R.items[s], cycle_reach_visitor, &R);
        if (R.oom) {
            ok = false;
            break;
        }
    }
    /* Pre-size the scanBlack frontier to |R| so passes B-D never allocate. */
    if (ok && R.count > 0) {
        work.items = (XrGCHeader **) xr_malloc((size_t) R.count * sizeof(XrGCHeader *));
        if (!work.items)
            ok = false;
        else
            work.cap = R.count;
    }

    if (!ok) {
        /* Allocation failed before any mutation: reset colors and skip this
         * collection (the cycle stays, retried on a later collect). */
        for (uint32_t i = 0; i < R.count; i++)
            set_color(R.items[i], COLOR_BLACK);
    } else if (R.count > 0) {
        /* Pass B: trial-decrement every internal edge once. */
        for (uint32_t i = 0; i < R.count; i++)
            visit_children(R.items[i], mark_gray_dec_visitor, NULL);

        /* Pass C: nodes with a surviving external reference are live. */
        for (uint32_t i = 0; i < R.count; i++) {
            XrGCHeader *obj = R.items[i];
            if (get_color(obj) == COLOR_GRAY && obj->refcount >= 0)
                scan_black_flat(obj, &work);
        }
        for (uint32_t i = 0; i < R.count; i++) {
            if (get_color(R.items[i]) == COLOR_GRAY)
                set_color(R.items[i], COLOR_WHITE); /* dead cycle member */
        }

        /* Pass D: restore WHITE out-edges so the destroy cascade drops each
         * edge exactly once, then destroy. The XR_OBJ_DEAD guard in
         * xr_coro_gc_rc_destroy makes the cascade idempotent across edges. */
        for (uint32_t i = 0; i < R.count; i++) {
            if (get_color(R.items[i]) == COLOR_WHITE)
                visit_children(R.items[i], restore_edge_visitor, NULL);
        }
        for (uint32_t i = 0; i < R.count; i++) {
            XrGCHeader *obj = R.items[i];
            if (get_color(obj) == COLOR_WHITE && !(obj->extra & XR_OBJ_DEAD)) {
                freed++;
                xr_coro_gc_rc_destroy(gc, obj);
            }
        }

        /* Reset colors on survivors so a stale GRAY/WHITE cannot mislead the
         * next collection's "in R" test. */
        for (uint32_t i = 0; i < R.count; i++) {
            XrGCHeader *obj = R.items[i];
            if (!(obj->extra & XR_OBJ_DEAD))
                set_color(obj, COLOR_BLACK);
        }
    }

    xr_free(R.items);
    xr_free(work.items);

    /* Reset the roots list. Surviving (non-dead) entries get their _rsv
     * sentinel back so future RC decrements can re-track them; without
     * this, add_root would refuse them forever and remove_root could
     * corrupt the next roots array through a stale index. Iterate the
     * CURRENT count: destructors run above may have added/removed roots. */
    for (uint32_t i = 0; i < gc->cycle_root_count; i++) {
        XrGCHeader *obj = gc->cycle_roots[i];
        if (obj && !(obj->extra & XR_OBJ_DEAD))
            obj->_rsv = XR_CYCLE_NOT_IN_ROOTS;
    }
    gc->cycle_root_count = 0;

    /* Adaptive threshold (Nim ORC feedback): a productive collect (reclaimed
     * at least half the scanned roots) lowers the threshold so we collect more
     * eagerly; an unproductive one raises it to avoid churn. Bounded below. */
    if (freed * 2u >= n) {
        uint32_t lowered = gc->cycle_collect_threshold * 2u / 3u;
        gc->cycle_collect_threshold =
            lowered < XR_CYCLE_COLLECT_THRESHOLD_MIN ? XR_CYCLE_COLLECT_THRESHOLD_MIN : lowered;
    } else {
        gc->cycle_collect_threshold += gc->cycle_collect_threshold / 2u;
    }

    gc->cycle_collecting = 0;

    /* Cycle collection just freed dead cycles; reclaim any blocks that became
     * fully dead so their memory can be reused by any size class. */
    xr_coro_gc_reclaim_blocks(gc);
}
