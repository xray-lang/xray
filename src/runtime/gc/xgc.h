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
 *   - Per-Coroutine GC: each coroutine has isolated heap (XrCoroGC)
 * - incremental Mark-Sweep with Arena allocation
 *   - Bulk deallocation when coroutine ends (Arena bulk free)
 *   - System heap for global objects (Class, Module) - not GC'd
 *
 * ALLOCATION PATH:
 *   xr_alloc(coro, size, type) -> coro->coro_gc (Mark-Sweep GC)
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

XR_FUNC void* xr_alloc(struct XrCoroutine *coro, size_t size, uint8_t type);
XR_FUNC struct XrCoroutine* xr_current_coro(struct XrayIsolate *X);

/* ========== Debug ========== */

#define xr_gc_stats(gc)        xr_gc_printstats(gc)

XR_FUNC void xr_gc_header_print(XrGCHeader *obj);

#endif // XGC_H
