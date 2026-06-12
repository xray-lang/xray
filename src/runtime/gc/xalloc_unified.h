/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xalloc_unified.h - Unified allocation API for Per-Coroutine GC
 *
 * KEY CONCEPT:
 *   Provides unified allocation interface for Arena Mark-Sweep GC.
 */

#ifndef XALLOC_UNIFIED_H
#define XALLOC_UNIFIED_H

#include <stddef.h>
#include <stdint.h>
#include "xgc_header.h"
#include "xcoro_gc.h"

/* ========== Forward Declarations ========== */

struct XrCoroutine;
struct XrayIsolate;

/* ========== Coroutine Accessor (avoids coro/ include) ========== */

XR_FUNC struct XrayIsolate *xr_coro_get_isolate(struct XrCoroutine *coro);

/* ========== Unified Allocation API ========== */

// Lazy initialization: create coro_gc on first heap allocation demand
XR_FUNC XrCoroGC *xr_coro_ensure_gc(struct XrCoroutine *coro);

// Allocate GC-managed object for coroutine
XR_FUNC void *xr_coro_alloc(struct XrCoroutine *coro, size_t size, uint8_t type);

// Get coroutine's GC context (for write barriers etc.)
XR_FUNC XrCoroGC *xr_coro_get_coro_gc(struct XrCoroutine *coro);

// Get current coroutine's GC context via TLS
XR_FUNC XrCoroGC *xr_current_coro_gc(void);

// Allocate raw byte buffer on coroutine's Immix GC heap (XR_TBLOB)
static inline void *xr_coro_alloc_blob(XrCoroGC *gc, size_t data_size) {
    if (!gc)
        return NULL;
    XrGCHeader *obj = xr_coro_gc_newobj(gc, XR_TBLOB, sizeof(XrGCHeader) + data_size);
    return obj ? (obj + 1) : NULL;
}

// Release a blob allocated by xr_coro_alloc_blob back to the owning
// coroutine's RC freelist. RC owns reclamation (tracing is retired, there
// is no sweep), so containers MUST release their backing blob explicitly
// when they are destroyed or when they reallocate their storage —
// otherwise the blob stays live until coroutine teardown.
static inline void xr_coro_free_blob(XrCoroGC *gc, void *data) {
    if (!gc || !data)
        return;
    XrGCHeader *obj = ((XrGCHeader *) data) - 1;
    XR_DCHECK(XR_GC_GET_TYPE(obj) == XR_TBLOB, "free_blob: not a blob");
    xr_coro_gc_rc_destroy(gc, obj);
}

// Write barrier for container modifications
XR_FUNC void xr_coro_write_barrier(struct XrCoroutine *coro, XrGCHeader *parent, XrGCHeader *child);

// Back barrier for container bulk modifications
XR_FUNC void xr_coro_write_barrier_back(struct XrCoroutine *coro, XrGCHeader *obj);

#endif  // XALLOC_UNIFIED_H
