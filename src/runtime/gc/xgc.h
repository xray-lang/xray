/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xgc.h - Garbage Collector public API
 *
 * KEY CONCEPT:
 *   - Per-coroutine Immix mark-region GC (XrCoroGC) is the primary heap
 *     for runtime objects. See xcoro_gc.h for the full state machine,
 *     tri-color invariants, and block/line layout.
 *   - Isolate-level fixedgc (XrGC) is a malloc-backed linked list used
 *     for bootstrap, fallback, and a small set of fixed-lifetime objects
 *     (e.g. enum metadata, bound methods). It does not run mark/sweep;
 *     destroy hooks are invoked once at isolate cleanup.
 *   - System heap (xsysheap) holds class metadata and shared/refcounted
 *     objects (channels, deep-copied shared values). These are not GC'd.
 *
 * ALLOCATION PATH:
 *   xr_alloc(coro, size, type) routes to coro->coro_gc when available,
 *   otherwise falls back to the isolate fixedgc. Most callers should
 *   resolve a coroutine via xr_current_coro(X) and pass it explicitly.
 */

#ifndef XGC_H
#define XGC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

// GC implementation
#include "xgc_internal.h"
#include "../value/xvalue.h"

/* ========== Unified Allocation Interface ========== */

// Forward declarations
struct XrCoroutine;
struct XrayIsolate;

XR_FUNC void *xr_alloc(struct XrCoroutine *coro, size_t size, uint8_t type);
XR_FUNC struct XrCoroutine *xr_current_coro(struct XrayIsolate *X);

/* ========== Debug ========== */

#define xr_gc_stats(gc) xr_gc_printstats(gc)

XR_FUNC void xr_gc_header_print(XrGCHeader *obj);

#endif  // XGC_H
