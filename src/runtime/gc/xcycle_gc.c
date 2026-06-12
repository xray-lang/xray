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
 *   1. Potential roots: objects whose RC decremented but stayed > 0.
 *   2. markGray: trial-decrement all children from each root.
 *   3. scan: if root RC > 0 after trial decrement, it is live (scanBlack
 *      restores children). Otherwise mark white (dead cycle member).
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

/* Threshold: only run the collector when root count exceeds this. */
#define XR_CYCLE_COLLECT_THRESHOLD 128

/* Color encoding for trial deletion (extra bits 8-9, see xgc_header.h).
 * Every reachable object is recolored before its color is read in each
 * phase, so stale colors from a previous collect round are harmless. */
#define COLOR_BLACK 0   /* definitely live */
#define COLOR_GRAY 1    /* trial-decremented, unknown liveness */
#define COLOR_WHITE 2   /* presumed dead (cycle garbage) */
#define COLOR_FREEING 3 /* white, queued in the collect buffer */

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
            if (xr_map_isdummy(map) || !map->node)
                break;
            uint32_t count = xr_map_sizenode(map);
            for (uint32_t i = 0; i < count; i++) {
                XrMapNode *node = &map->node[i];
                if (XR_MAP_NODE_EMPTY(node))
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
             * keep the cycle alive). Strings/blobs hold no RC edges. */
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

/* ========== Bacon-Rajan Trial Deletion ========== */

/* markGray: trial-decrement children's RC. Recurse into gray children. */
static void mark_gray_visitor(XrGCHeader *child, void *ctx) {
    (void) ctx;
    if (!child || (child->extra & XR_OBJ_DEAD) || !cycle_child_eligible(child))
        return;
    child->refcount--;
    if (get_color(child) != COLOR_GRAY) {
        set_color(child, COLOR_GRAY);
        visit_children(child, mark_gray_visitor, NULL);
    }
}

static void mark_gray(XrGCHeader *obj) {
    set_color(obj, COLOR_GRAY);
    visit_children(obj, mark_gray_visitor, NULL);
}

/* scanBlack: restore children's RC (object is confirmed live). */
static void scan_black_visitor(XrGCHeader *child, void *ctx) {
    (void) ctx;
    if (!child || (child->extra & XR_OBJ_DEAD) || !cycle_child_eligible(child))
        return;
    child->refcount++;
    if (get_color(child) != COLOR_BLACK) {
        set_color(child, COLOR_BLACK);
        visit_children(child, scan_black_visitor, NULL);
    }
}

static void scan_black(XrGCHeader *obj) {
    set_color(obj, COLOR_BLACK);
    visit_children(obj, scan_black_visitor, NULL);
}

/* scan: after markGray, check if root is still live. */
static void scan_visitor(XrGCHeader *child, void *ctx);

static void scan(XrGCHeader *obj) {
    if (get_color(obj) != COLOR_GRAY)
        return;
    if (obj->refcount > 0) {
        /* Still reachable from outside the cycle — restore. */
        scan_black(obj);
    } else {
        /* Presumed dead cycle member. */
        set_color(obj, COLOR_WHITE);
        visit_children(obj, scan_visitor, NULL);
    }
}

static void scan_visitor(XrGCHeader *child, void *ctx) {
    (void) ctx;
    if (!child || (child->extra & XR_OBJ_DEAD) || !cycle_child_eligible(child))
        return;
    scan(child);
}

/* ========== Collect Phase (restore-then-destroy) ========== */

/* Growable buffer of white objects. Buffering first means we never have to
 * traverse object payloads after any destructor/free has run. */
typedef struct {
    XrGCHeader **items;
    uint32_t count;
    uint32_t cap;
    bool oom;
} WhiteBuf;

static void white_buf_push(WhiteBuf *buf, XrGCHeader *obj) {
    if (buf->oom)
        return;
    if (buf->count >= buf->cap) {
        uint32_t new_cap = buf->cap ? buf->cap * 2 : 64;
        XrGCHeader **tmp = (XrGCHeader **) xr_realloc(buf->items, sizeof(XrGCHeader *) * new_cap);
        if (!tmp) {
            buf->oom = true;
            return;
        }
        buf->items = tmp;
        buf->cap = new_cap;
    }
    buf->items[buf->count++] = obj;
}

/* Gather all white objects reachable from `obj` into the buffer, recoloring
 * them FREEING as the visited marker. No RC mutation, no freeing here. */
static void collect_white_visitor(XrGCHeader *child, void *ctx);

static void collect_white(XrGCHeader *obj, WhiteBuf *buf) {
    if (get_color(obj) != COLOR_WHITE)
        return;
    if (obj->extra & XR_OBJ_DEAD)
        return;
    set_color(obj, COLOR_FREEING);
    white_buf_push(buf, obj);
    visit_children(obj, collect_white_visitor, buf);
}

static void collect_white_visitor(XrGCHeader *child, void *ctx) {
    if (!child || !cycle_child_eligible(child))
        return;
    collect_white(child, (WhiteBuf *) ctx);
}

/* Restore one out-edge of a white object (undo its markGray decrement).
 * Edges to ineligible children were never decremented, so skip them. */
static void restore_edge_visitor(XrGCHeader *child, void *ctx) {
    (void) ctx;
    if (!child || (child->extra & XR_OBJ_DEAD) || !cycle_child_eligible(child))
        return;
    child->refcount++;
}

/* OOM abort: recolor every still-white/freeing object black and restore its
 * out-edge decrements WITHOUT destroying anything. The cycle leaks (it can
 * be retried on a later collect), but no refcount is left understated. */
static void abort_restore_visitor(XrGCHeader *child, void *ctx);

static void abort_restore(XrGCHeader *obj) {
    uint8_t c = get_color(obj);
    if (c != COLOR_WHITE && c != COLOR_FREEING)
        return;
    set_color(obj, COLOR_BLACK);
    visit_children(obj, restore_edge_visitor, NULL);
    visit_children(obj, abort_restore_visitor, NULL);
}

static void abort_restore_visitor(XrGCHeader *child, void *ctx) {
    (void) ctx;
    if (!child || (child->extra & XR_OBJ_DEAD) || !cycle_child_eligible(child))
        return;
    abort_restore(child);
}

/* Main entry: run the Bacon-Rajan cycle collector on accumulated roots.
 * Called by gc.collect(). Runs unconditionally when there are roots. */
XR_FUNC void xr_coro_gc_fullgc(XrCoroGC *gc) {
    if (!gc)
        return;
    if (!gc->cycle_roots || gc->cycle_root_count == 0)
        return;

    gc->gc_count++;

    uint32_t n = gc->cycle_root_count;

    /* markGray all roots. */
    for (uint32_t i = 0; i < n; i++) {
        XrGCHeader *obj = gc->cycle_roots[i];
        if (obj && !(obj->extra & XR_OBJ_DEAD) && cycle_child_eligible(obj)) {
            set_color(obj, COLOR_BLACK); /* initialize */
            mark_gray(obj);
        }
    }

    /* scan: determine which are truly dead. */
    for (uint32_t i = 0; i < n; i++) {
        XrGCHeader *obj = gc->cycle_roots[i];
        if (obj && !(obj->extra & XR_OBJ_DEAD) && cycle_child_eligible(obj))
            scan(obj);
    }

    /* Gather the white set (no mutation yet). */
    WhiteBuf buf = {0};
    for (uint32_t i = 0; i < n; i++) {
        XrGCHeader *obj = gc->cycle_roots[i];
        if (obj && cycle_child_eligible(obj))
            collect_white(obj, &buf);
    }

    if (buf.oom) {
        /* Could not buffer the full white set: restore all trial decrements
         * and bail without freeing (leak-safe, retryable). */
        for (uint32_t i = 0; i < n; i++) {
            XrGCHeader *obj = gc->cycle_roots[i];
            if (obj && !(obj->extra & XR_OBJ_DEAD) && cycle_child_eligible(obj))
                abort_restore(obj);
        }
    } else {
        /* Restore every white object's out-edges, THEN run the normal
         * destroy cascade. After restoration the refcounts are exact, so
         * the type destructors drop each edge exactly once: live children
         * referenced from the dead cycle end up with a correct refcount,
         * and owned side buffers (map nodes, array data, ...) are freed.
         * The XR_OBJ_DEAD guard in xr_coro_gc_rc_destroy makes the
         * cascade idempotent across cycle edges. */
        for (uint32_t i = 0; i < buf.count; i++)
            visit_children(buf.items[i], restore_edge_visitor, NULL);
        for (uint32_t i = 0; i < buf.count; i++) {
            XrGCHeader *obj = buf.items[i];
            if (!(obj->extra & XR_OBJ_DEAD))
                xr_coro_gc_rc_destroy(gc, obj);
        }
    }
    xr_free(buf.items);

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
}
