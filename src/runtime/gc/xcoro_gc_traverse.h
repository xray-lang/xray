/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_gc_traverse.h - Type-specific GC traversal functions
 *
 * KEY CONCEPT:
 * Traversal functions for Mark-Sweep GC.
 *   Each type defines how to traverse its references.
 */

#ifndef XCORO_GC_TRAVERSE_H
#define XCORO_GC_TRAVERSE_H

#include "xcoro_gc.h"
#include "xgc_header.h"

// Forward declarations
struct XrArray;
struct XrMap;
struct XrSet;
struct XrJson;
struct XrClosure;
struct XrInstance;
struct XrIterator;
struct XrCoroutine;

/* ========== Type Traversal Functions ========== */

XR_FUNC void xr_gc_traverse_array(XrCoroGC *gc, struct XrArray *arr);
XR_FUNC void xr_gc_traverse_map(XrCoroGC *gc, struct XrMap *map);
XR_FUNC void xr_gc_traverse_set(XrCoroGC *gc, struct XrSet *set);
XR_FUNC void xr_coro_gc_traverse_json(XrCoroGC *gc, struct XrJson *json);
XR_FUNC void xr_gc_traverse_closure(XrCoroGC *gc, struct XrClosure *closure);
XR_FUNC void xr_gc_traverse_instance(XrCoroGC *gc, struct XrInstance *inst);
XR_FUNC void xr_gc_traverse_iterator(XrCoroGC *gc, struct XrIterator *iter);

/* ========== Generic Traversal Dispatcher ========== */

XR_FUNC void xr_gc_traverse_object(XrCoroGC *gc, XrGCHeader *obj);

#endif // XCORO_GC_TRAVERSE_H
