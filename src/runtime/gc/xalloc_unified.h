/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xalloc_unified.h - Unified allocation API for coroutine heap
 *
 * KEY CONCEPT:
 *   Provides the allocation bridge from coroutine/runtime code to the
 *   per-coroutine Region + RC heap.
 */

#ifndef XALLOC_UNIFIED_H
#define XALLOC_UNIFIED_H

#include <stddef.h>
#include <stdint.h>
#include "xobj_header.h"
#include "xcoro_heap.h"

/* ========== Forward Declarations ========== */

struct XrCoroutine;
struct XrayIsolate;

/* ========== Coroutine Accessor (avoids coro/ include) ========== */

XR_FUNC struct XrayIsolate *xr_coro_get_isolate(struct XrCoroutine *coro);

/* ========== Unified Allocation API ========== */

// Lazy initialization: create heap on first heap allocation demand
XR_FUNC XrCoroHeap *xr_coro_ensure_heap(struct XrCoroutine *coro);

// Allocate an RC-managed object for a coroutine
XR_FUNC void *xr_coro_alloc(struct XrCoroutine *coro, size_t size, uint8_t type);

// Get coroutine's heap context
XR_FUNC XrCoroHeap *xr_coro_get_heap(struct XrCoroutine *coro);

// Get current coroutine's heap context via TLS
XR_FUNC XrCoroHeap *xr_current_coro_heap(void);

// Allocate raw byte buffer on the coroutine's Region heap (XR_TBLOB)
static inline void *xr_coro_alloc_blob(XrCoroHeap *heap, size_t data_size) {
    if (!heap)
        return NULL;
    XrObjHeader *obj = xr_coro_heap_new_obj(heap, XR_TBLOB, sizeof(XrObjHeader) + data_size);
    return obj ? (obj + 1) : NULL;
}

// Release a blob allocated by xr_coro_alloc_blob back to the owning
// coroutine's RC freelist. RC owns reclamation (tracing is retired, there
// is no sweep), so containers MUST release their backing blob explicitly
// when they are destroyed or when they reallocate their storage —
// otherwise the blob stays live until coroutine teardown.
static inline void xr_coro_free_blob(XrCoroHeap *heap, void *data) {
    if (!heap || !data)
        return;
    XrObjHeader *obj = ((XrObjHeader *) data) - 1;
    XR_DCHECK(XR_OBJ_GET_TYPE(obj) == XR_TBLOB, "free_blob: not a blob");
    xr_coro_heap_destroy_obj(heap, obj);
}

#endif  // XALLOC_UNIFIED_H
