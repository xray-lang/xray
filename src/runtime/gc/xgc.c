/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xgc.c - Simplified global GC implementation
 *
 * KEY CONCEPT:
 *   Manages fixed GC objects and type function registration:
 *   1. Fixed objects: lifetime equals program runtime (e.g. main coroutine)
 *   2. Type functions: traverse/destroy/getgclist
 *
 * Note: Runtime objects allocated on coroutine heap (Per-Coroutine GC arch)
 *
 * RELATED MODULES:
 *   - xcoro_gc.c: Per-coroutine GC implementation
 */

#include "xgc_internal.h"
#include "xcoro_gc.h"
#include "xalloc_unified.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "../value/xvalue.h"
#include "../xisolate_api.h"
#include "../xisolate_internal.h"
#include "../../coro/xcoroutine.h"
#include "../../coro/xworker.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../base/xmalloc.h"

/* ========== Compile-Time Type Function Tables (.rodata) ========== */

const XrGCDestroyFn g_destroy_funcs[XGC_MAX_TYPES] = {
    [XR_TARRAY]         = xr_gc_destroy_array,
    [XR_TMAP]           = xr_gc_destroy_map,
    [XR_TSET]           = xr_gc_destroy_set,
    [XR_TSTRINGBUILDER] = xr_gc_destroy_stringbuilder,
    [XR_TCHANNEL]       = xr_gc_destroy_channel,
    [XR_TCOROUTINE]     = xr_gc_destroy_coroutine,
    [XR_TREGEX]         = regex_object_destroy,
    [XR_TLOGGER]        = xr_gc_destroy_logger,
    [XR_TJSON]          = xr_gc_destroy_json,
    [XR_TTASK]          = xr_gc_destroy_task,
    [XR_TENUM_TYPE]     = xr_gc_destroy_enum_type,
    [XR_TENUM_VALUE]    = xr_gc_destroy_enum_value,
};

/* ========== GC State ========== */

#define xr_gc_gettype(o)  XR_GC_GET_TYPE(o)

static XrGCDestroyFn get_destroy_func(uint8_t type) {
    return (type < XGC_MAX_TYPES) ? g_destroy_funcs[type] : NULL;
}

/* ========== Init/Cleanup ========== */

void xr_gc_init(XrGC *gc, struct XrayIsolate *isolate) {
    XR_DCHECK(gc != NULL, "gc_init: NULL gc");
    XR_DCHECK(isolate != NULL, "gc_init: NULL isolate");
    memset(gc, 0, sizeof(XrGC));
    gc->isolate = isolate;
    gc->gcstate = XGC_IDLE;
}

void xr_gc_cleanup(XrGC *gc) {
    XR_DCHECK(gc != NULL, "gc_cleanup: NULL gc");
    // Free fixed GC objects
    XrGCHeader *obj = gc->fixedgc;
    while (obj != NULL) {
        XrGCHeader *next = obj->gc_next;
        uint8_t type = xr_gc_gettype(obj);
        XrGCDestroyFn destroy = get_destroy_func(type);
        if (!destroy && gc->isolate) {
            destroy = gc->isolate->ext_destroy_funcs[type];
        }
        if (destroy != NULL) {
            destroy(obj, NULL);
        }
        xr_free(obj);
        obj = next;
    }
    gc->fixedgc = NULL;

    gc->object_count = 0;
    gc->totalbytes = 0;
}

/* ========== Allocation (Only for fixed objects during initialization) ========== */

void* xr_gc_alloc(XrGC *gc, size_t size, uint8_t type) {
    XR_DCHECK(gc != NULL, "gc_alloc: NULL gc");
    XR_DCHECK(size >= sizeof(XrGCHeader), "gc_alloc: size too small");
    XR_DCHECK(type < XGC_MAX_TYPES, "gc_alloc: invalid GC type");
    // Global GC: Allocate fixed objects using malloc
    // Note: Runtime objects should use xr_alloc() or xr_coro_gc_alloc()
    XrGCHeader *obj = (XrGCHeader*)xr_malloc(size);
    if (obj) {
        obj->type = type;
        obj->marked = 0;
        obj->extra = 0;
        obj->objsize = (uint32_t)size;
        // Link into fixedgc list so xr_gc_cleanup can free all objects
        obj->gc_next = gc->fixedgc;
        gc->fixedgc = obj;
        gc->totalbytes += (int64_t)size;
        gc->object_count++;
    }
    return obj;
}

XrGCHeader* xr_gc_newobj(XrGC *gc, uint8_t type, size_t size) {
    XR_DCHECK(gc != NULL, "gc_newobj: NULL gc");
    return (XrGCHeader*)xr_gc_alloc(gc, size, type);
}


/* ========== Debug ========== */

void xr_gc_printstats(XrGC *gc) {
    XR_DCHECK(gc != NULL, "gc_printstats: NULL gc");
    printf("=== XrGC Stats (Global/Fixed) ===\n");
    printf("Objects: %zu\n", gc->object_count);
    printf("Total bytes: %lld\n", (long long)gc->totalbytes);
    printf("=================================\n");
}

// GC Header Debug Print
void xr_gc_header_print(XrGCHeader *obj) {
    if (!obj) {
        printf("GC Header: NULL\n");
        return;
    }
    printf("GC Header:\n");
    printf("  gc_next: %p\n", (void*)obj->gc_next);
    printf("  type: %d\n", obj->type);
    printf("  marked: 0x%02x\n", obj->marked);
    printf("  objsize: %u\n", obj->objsize);
    printf("  white: %d\n", xr_gc_iswhite(obj) ? 1 : 0);
    printf("  black: %d\n", xr_gc_isblack(obj) ? 1 : 0);
    printf("  gray: %d\n", xr_gc_isgray(obj) ? 1 : 0);
}

/* ========== Unified Allocation Interface ========== */

void* xr_alloc(struct XrCoroutine *coro, size_t size, uint8_t type) {
    XR_DCHECK(coro != NULL, "xr_alloc: coro must not be NULL");
    XR_DCHECK(((XrGCHeader*)coro)->type == XR_TCOROUTINE,
              "xr_alloc: coro is not XrCoroutine (caller passed wrong type)");
    if (!coro) return NULL;

    // Lazy coro_gc creation on first heap allocation
    XrCoroGC *gc = xr_coro_ensure_gc(coro);
    if (gc) {
        XrGCHeader *obj = xr_coro_gc_newobj(gc, type, size);
        if (obj) return obj;
        xr_log_warning("gc", "xr_alloc: coro_gc allocation failed for type=%d size=%zu",
                type, size);
        return NULL;
    }

    // Fallback: use isolate's global GC (needed during early isolate init
    // when coro_gc creation fails due to missing worker/machine)
    if (coro->isolate) {
        return xr_gc_alloc(&coro->isolate->gc, size, type);
    }
    return NULL;
}
