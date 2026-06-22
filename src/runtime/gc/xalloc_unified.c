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
#include "xcoro_heap.h"

XrCoroHeap *xr_coro_ensure_heap(struct XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "coro_ensure_gc: NULL coro");
    if (coro->heap)
        return coro->heap;
    coro->heap = xr_coro_heap_create(coro);
    return coro->heap;
}

void *xr_coro_alloc(struct XrCoroutine *coro, size_t size, uint8_t type) {
    XR_DCHECK(size > 0, "coro_alloc: zero size");
    XR_DCHECK(type < XR_OBJ_TYPE_MAX, "coro_alloc: invalid object type");
    if (!coro || !coro->heap)
        return NULL;
    XrObjHeader *obj = xr_coro_heap_new_obj(coro->heap, type, size);
    return obj ? (obj + 1) : NULL;
}

XrCoroHeap *xr_coro_get_heap(struct XrCoroutine *coro) {
    return coro ? coro->heap : NULL;
}

XrCoroHeap *xr_current_coro_heap(void) {
    XrWorker *w = xr_current_worker();
    if (w && w->m && w->m->current_coro)
        return w->m->current_coro->heap;
    return NULL;
}

void xr_coro_write_barrier(struct XrCoroutine *coro, XrObjHeader *parent, XrObjHeader *child) {
    /* Retired: RC owns reclamation, no tri-color invariant to maintain. */
    (void) coro;
    (void) parent;
    (void) child;
}

void xr_coro_write_barrier_back(struct XrCoroutine *coro, XrObjHeader *obj) {
    /* Retired: RC owns reclamation, no tri-color invariant to maintain. */
    (void) coro;
    (void) obj;
}

XrayIsolate *xr_coro_get_isolate(struct XrCoroutine *coro) {
    return coro ? coro->isolate : NULL;
}
