/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xgc.h - Heap allocation and object lifecycle API
 *
 * KEY CONCEPT:
 *   - Per-coroutine Region bump heap (XrCoroHeap) is the primary heap for
 *     runtime objects. Reclamation is compile-time reference counting: a
 *     per-size-class freelist reuses dropped blocks, whole-block reclaim
 *     returns emptied blocks, and a Bacon-Rajan cycle collector handles
 *     cycles. There is no tracing/mark-sweep. See xcoro_heap.h.
 *   - Isolate-level fixed heap (XrFixedHeap) is a malloc-backed linked list used
 *     for bootstrap, fallback, and a small set of fixed-lifetime objects
 *     (e.g. enum metadata, bound methods). Destroy hooks are invoked once
 *     at isolate cleanup.
 *   - System heap (xsysheap) holds class metadata and shared/refcounted
 *     objects (channels, deep-copied shared values). These are not GC'd.
 *
 * ALLOCATION PATH:
 *   xr_alloc(coro, size, type) routes to coro->heap when available,
 *   otherwise falls back to the isolate fixed heap. Most callers should
 *   resolve a coroutine via xr_current_coro(X) and pass it explicitly.
 */

#ifndef XGC_H
#define XGC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

// Heap implementation
#include "xgc_internal.h"
#include "../value/xvalue.h"

/* ========== Unified Allocation Interface ========== */

// Forward declarations
struct XrCoroutine;
struct XrayIsolate;

XR_FUNC void *xr_alloc(struct XrCoroutine *coro, size_t size, uint8_t type);
XR_FUNC struct XrCoroutine *xr_current_coro(struct XrayIsolate *X);

/* ========== Debug ========== */

#define xr_fixed_heap_stats(heap) xr_fixed_heap_print_stats(heap)

XR_FUNC void xr_obj_header_print(XrObjHeader *obj);

#endif  // XGC_H
