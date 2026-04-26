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
#include "xgc_internal.h"  // Per-type traverse function declarations + g_type_ops

/* ========== Generic Traversal Dispatcher ==========
 *
 * Resolves the type's traverse callback through g_type_ops (compile-time
 * types) or per-isolate ext_traverse_funcs (extension types). Per-type
 * traverse function declarations live in xgc_internal.h alongside the
 * destroy ones so a single header pulls in the entire ops table contract. */

XR_FUNC void xr_gc_traverse_object(XrCoroGC *gc, XrGCHeader *obj);

#endif // XCORO_GC_TRAVERSE_H
