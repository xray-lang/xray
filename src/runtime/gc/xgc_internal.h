/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xgc_internal.h - Global GC interface (minimal)
 *
 * KEY CONCEPT:
 *   - Runtime objects allocated in coroutine heaps (xcoro_gc.c)
 *   - This file manages: fixedgc list, type function registration
 */

#ifndef XGC_INTERNAL_H
#define XGC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "../../os/os_thread.h"

#ifndef XR_VALUE_DEFINED
typedef struct XrValue XrValue;
#endif

// XR_THREAD_LOCAL is provided by base/xdefs.h via xgc_header.h.
#include "xgc_header.h"

/* ========== GC State ========== */

#define XGC_IDLE 0

/* ========== Marked Field Access ========== */

#define xr_gc_getmarked(o) ((o)->marked)
#define xr_gc_setmarked(o, m) ((o)->marked = (uint8_t) (m))

/*
 * Color bit definitions are ONLY in xcoro_gc.h (dual-white).
 * Global GC manages fixedgc objects that live forever and never
 * participate in mark-sweep, so they don't need color marking.
 */

/* ========== GC Main Structure ========== */

struct XrayIsolate;
typedef struct XrGC XrGC;
typedef struct XrCoroGC XrCoroGC;

// Maximum GC type ID
#define XGC_MAX_TYPES 64

/* ========== Type Function Types (must be before XrGC) ========== */

struct XrGC;  // Forward declaration
struct XrayIsolate;
struct XrCopyContext;

typedef void (*XrGCDestroyFn)(XrGCHeader *obj, XrCoroGC *owning_gc);
typedef void (*XrGCTraverseFn)(XrCoroGC *gc, XrGCHeader *obj);
typedef XrValue (*XrGCDeepCopyFn)(struct XrCopyContext *ctx, XrGCHeader *obj);
typedef XrValue (*XrGCToSharedFn)(struct XrayIsolate *X, XrGCHeader *obj);
typedef struct XrGCHeader **(*XrGCGetGCListFn)(XrGCHeader *obj);

/* ========== Per-Type Operations Table ==========
 *
 * Single source of truth for every compile-time GC type. Each slot
 * either provides a callback or stays NULL to express the absence of
 * that capability:
 *
 *   destroy   == NULL  -> leaf-and-resourceless: nothing to free.
 *   traverse  == NULL  -> no GC-traced children (range, datetime, ...).
 *   deep_copy == NULL  -> not deep-copyable across coroutines; the
 *                          dispatcher passes the value through unchanged.
 *   to_shared == NULL  -> no shared-storage form available; xr_to_shared
 *                          returns the value unchanged.
 *
 * The booleans previously expressed as HAS_REFS_BITMAP and
 * NEEDS_FINALIZE_BITMAP are derived directly from the pointers —
 * has_refs(t) == traverse != NULL, needs_finalize(t) == destroy != NULL
 * — eliminating the three locations where the same capability used to
 * be encoded.
 *
 * Extension types (registered via xr_register_extension_destroy /
 * xr_register_extension_traverse) live in per-isolate tables on
 * XrayIsolate and are consulted as a fallback when this table's slot
 * is empty. */
typedef struct {
    XrGCDestroyFn destroy;     // Release malloc-backed side resources
    XrGCTraverseFn traverse;   // Mark GC-traced children
    XrGCDeepCopyFn deep_copy;  // Cross-coroutine deep copy (Immix heap)
    XrGCToSharedFn to_shared;  // Cross-coroutine shared/refcount copy
} XrTypeOps;

typedef struct XrGC {
    uint8_t gcstate;
    uint8_t _pad[7];
    struct XrayIsolate *isolate;
    int64_t totalbytes;
    XrGCHeader *fixedgc;  // Fixed objects (compile-time)
    size_t object_count;
} XrGC;

/* ========== Core API ========== */

XR_FUNC void xr_gc_init(XrGC *gc, struct XrayIsolate *isolate);
XR_FUNC void xr_gc_cleanup(XrGC *gc);
XR_FUNC void *xr_gc_alloc(XrGC *gc, size_t size, uint8_t type);
XR_FUNC XrGCHeader *xr_gc_newobj(XrGC *gc, uint8_t type, size_t size);

/* ========== Compile-Time Type Function Tables ========== */

// Single per-type operations table. Defined in xgc.c, referenced by
// xr_gc_cleanup, xcoro_gc.c (sweep / finalize) and xcoro_gc_traverse.c
// (mark dispatcher). One entry per XGC_MAX_TYPES slot.
extern const XrTypeOps g_type_ops[XGC_MAX_TYPES];

// Destroy functions (non-static, referenced by const tables)
XR_FUNC void xr_gc_destroy_array(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_map(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_set(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_channel(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_coroutine(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_instance(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_task(XrGCHeader *obj, XrCoroGC *owning_gc);
// NetConn / NetListener destroy handled by native body descriptors.

// Traverse functions (non-static, referenced by g_type_ops).
// Each marks the type's GC-traced children via xr_coro_gc_markvalue /
// xr_coro_gc_markobject. All share the XrGCTraverseFn signature so
// they can populate the table without a cast. Definitions live in
// xcoro_gc_traverse.c.
XR_FUNC void xr_gc_traverse_array(XrCoroGC *gc, XrGCHeader *obj);
XR_FUNC void xr_gc_traverse_map(XrCoroGC *gc, XrGCHeader *obj);
XR_FUNC void xr_gc_traverse_set(XrCoroGC *gc, XrGCHeader *obj);
XR_FUNC void xr_gc_traverse_closure(XrCoroGC *gc, XrGCHeader *obj);
XR_FUNC void xr_gc_traverse_instance(XrCoroGC *gc, XrGCHeader *obj);
XR_FUNC void xr_gc_traverse_cell(XrCoroGC *gc, XrGCHeader *obj);
XR_FUNC void xr_gc_traverse_bound_method(XrCoroGC *gc, XrGCHeader *obj);
XR_FUNC void xr_gc_traverse_module(XrCoroGC *gc, XrGCHeader *obj);
XR_FUNC void xr_gc_traverse_error(XrCoroGC *gc, XrGCHeader *obj);
XR_FUNC void xr_gc_traverse_task(XrCoroGC *gc, XrGCHeader *obj);

/* ========== Debug API ========== */

XR_FUNC void xr_gc_printstats(XrGC *gc);

#endif  // XGC_INTERNAL_H
