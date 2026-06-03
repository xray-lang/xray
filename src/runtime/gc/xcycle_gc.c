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
 *   4. collectWhite: free all white objects.
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
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"

#include <string.h>

/* Initial capacity for cycle_roots (lazy; NULL until first add). */
#define XR_CYCLE_ROOTS_INIT_CAP 64

/* Threshold: only run the collector when root count exceeds this. */
#define XR_CYCLE_COLLECT_THRESHOLD 128

/* Color encoding for trial deletion (stored in low 2 bits of _rsv
 * during collection; restored to root_idx after). */
#define COLOR_BLACK 0 /* definitely live */
#define COLOR_GRAY 1  /* trial-decremented, unknown liveness */
#define COLOR_WHITE 2 /* presumed dead (cycle garbage) */

/* Encode color into the _rsv field (keeping upper bits for root_idx). */
static inline void set_color(XrGCHeader *obj, uint8_t color) {
    obj->_rsv = (obj->_rsv & 0xFFFFFF00u) | color;
}

static inline uint8_t get_color(const XrGCHeader *obj) {
    return (uint8_t) (obj->_rsv & 0xFFu);
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
        default:
            /* Other types (string, closure, etc.) do not hold arbitrary
             * RC references that could form user-visible cycles. */
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
    if (!child || (child->extra & XR_OBJ_DEAD))
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
    if (!child || (child->extra & XR_OBJ_DEAD))
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
    if (!child)
        return;
    scan(child);
}

/* collectWhite: free dead cycle members. */
static void collect_white_visitor(XrGCHeader *child, void *ctx);

static void collect_white(XrGCHeader *obj, XrCoroGC *gc) {
    if (get_color(obj) != COLOR_WHITE)
        return;
    if (obj->extra & XR_OBJ_DEAD)
        return;

    /* Mark as collected to avoid double-free. */
    set_color(obj, COLOR_BLACK);
    obj->extra |= XR_OBJ_DEAD;

    /* Visit children first (they may also be white cycle members). */
    visit_children(obj, collect_white_visitor, gc);

    /* Free the object. */
    if (gc->object_count > 0)
        gc->object_count--;
    xr_coro_gc_rc_free(gc, obj);
}

static void collect_white_visitor(XrGCHeader *child, void *ctx) {
    if (!child)
        return;
    collect_white(child, (XrCoroGC *) ctx);
}

/* Main entry: run the Bacon-Rajan cycle collector on accumulated roots.
 * Called by gc.collect(). Runs unconditionally when there are roots. */
XR_FUNC void xr_coro_gc_fullgc(XrCoroGC *gc) {
    if (!gc)
        return;
    if (!gc->cycle_roots || gc->cycle_root_count == 0)
        return;

    gc->gc_count++;

    /* Save root_idx values (we reuse _rsv for color during collection). */
    uint32_t n = gc->cycle_root_count;

    /* markGray all roots. */
    for (uint32_t i = 0; i < n; i++) {
        XrGCHeader *obj = gc->cycle_roots[i];
        if (obj && !(obj->extra & XR_OBJ_DEAD)) {
            set_color(obj, COLOR_BLACK); /* initialize */
            mark_gray(obj);
        }
    }

    /* scan: determine which are truly dead. */
    for (uint32_t i = 0; i < n; i++) {
        XrGCHeader *obj = gc->cycle_roots[i];
        if (obj && !(obj->extra & XR_OBJ_DEAD))
            scan(obj);
    }

    /* collectWhite: free dead cycle members. */
    for (uint32_t i = 0; i < n; i++) {
        XrGCHeader *obj = gc->cycle_roots[i];
        if (obj)
            collect_white(obj, gc);
    }

    /* Reset roots list (all processed). */
    gc->cycle_root_count = 0;

    /* Restore _rsv = XR_CYCLE_NOT_IN_ROOTS for surviving roots
     * (they were already removed by collectWhite or scanBlack set them
     * back to BLACK; either way they are no longer in the roots array). */
}
