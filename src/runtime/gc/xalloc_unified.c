/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xalloc_unified.c - Unified allocation API implementation
 *
 * KEY CONCEPT:
 *   Implements coroutine-aware allocation functions that need full
 *   XrCoroutine/XrWorker type definitions. Extracted from the header
 *   to break gc/(L2) -> coro/(L3) layer dependency.
 */

#include "xalloc_unified.h"
#include "../../base/xchecks.h"
#include "../../coro/xcoroutine.h"
#include "../../coro/xworker.h"
#include "xcoro_gc.h"

XrCoroGC *xr_coro_ensure_gc(struct XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "coro_ensure_gc: NULL coro");
    if (coro->coro_gc)
        return coro->coro_gc;
    coro->coro_gc = xr_coro_gc_create(coro, NULL);
    return coro->coro_gc;
}

void *xr_coro_alloc(struct XrCoroutine *coro, size_t size, uint8_t type) {
    XR_DCHECK(size > 0, "coro_alloc: zero size");
    XR_DCHECK(type < XGC_MAX_TYPES, "coro_alloc: invalid GC type");
    if (!coro || !coro->coro_gc)
        return NULL;
    XrGCHeader *obj = xr_coro_gc_newobj(coro->coro_gc, type, size);
    return obj ? (obj + 1) : NULL;
}

XrCoroGC *xr_coro_get_coro_gc(struct XrCoroutine *coro) {
    return coro ? coro->coro_gc : NULL;
}

XrCoroGC *xr_current_coro_gc(void) {
    XrWorker *w = xr_current_worker();
    if (w && w->m && w->m->current_coro)
        return w->m->current_coro->coro_gc;
    return NULL;
}

void xr_coro_write_barrier(struct XrCoroutine *coro, XrGCHeader *parent, XrGCHeader *child) {
    XR_DCHECK(parent != NULL, "write_barrier: NULL parent");
    if (coro && coro->coro_gc && child) {
        xr_coro_gc_barrier(coro->coro_gc, parent, child);
    }
}

void xr_coro_write_barrier_back(struct XrCoroutine *coro, XrGCHeader *obj) {
    XR_DCHECK(obj != NULL, "write_barrier_back: NULL obj");
    if (coro && coro->coro_gc) {
        xr_coro_gc_barrierback(coro->coro_gc, obj);
    }
}

XrayIsolate *xr_coro_get_isolate(struct XrCoroutine *coro) {
    return coro ? coro->isolate : NULL;
}
