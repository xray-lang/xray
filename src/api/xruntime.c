/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xruntime.c - Unified runtime API implementation
 *
 * NOTE: These APIs are currently unused, reserved for future extension.
 * Object allocation should use xr_alloc() directly.
 */

#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/value/xtype.h"
#include "../runtime/gc/xgc.h"
#include "xruntime.h"

/* ========== Memory Allocation (currently unused) ========== */

// Allocate memory from global GC heap.
// NOTE: Currently unused, objects use xr_alloc directly.
void* xray_alloc(XrayIsolate *X, size_t size) {
    xray_api_checkr(X != NULL, "xray_alloc: NULL isolate", NULL);
    xray_api_checkr(size > 0, "xray_alloc: zero size", NULL);
    return xr_gc_alloc(xr_isolate_get_gc(X), size, 0);
}

// Reallocate memory.
// NOTE: Currently unused. Coroutine heap uses Cheney GC (no in-place expansion).
// This function is only for non-GC-managed memory (e.g., xr_malloc-allocated data).
void* xray_realloc(XrayIsolate *X, void *ptr, size_t old_size, size_t new_size) {
    xray_api_checkr(X != NULL, "xray_realloc: NULL isolate", NULL);
    (void)old_size;
    return xr_realloc(ptr, new_size);
}

// Free memory.
// NOTE: GC manages memory automatically, this is a no-op.
void xray_free(XrayIsolate *X, void *ptr, size_t size) {
    xray_api_check(X != NULL, "xray_free: NULL isolate");
    (void)ptr;
    (void)size;
    // GC automatically reclaims unmarked objects, no manual free needed
}

