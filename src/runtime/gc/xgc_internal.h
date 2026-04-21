/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xgc_internal.h - Global GC interface (minimal)
 *
 * KEY CONCEPT:
 *   - Runtime objects allocated in coroutine heaps (xcoro_gc.c)
 *   - This file manages: fixedgc list, type function registration
 */

#ifndef XGC_INTERNAL_H
#define XGC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>

#ifndef XR_VALUE_DEFINED
typedef struct XrValue XrValue;
#endif // Thread-local storage macro
#ifdef __GNUC__
    #define XR_THREAD_LOCAL __thread
#elif defined(_MSC_VER)
    #define XR_THREAD_LOCAL __declspec(thread)
#else
    #define XR_THREAD_LOCAL
#endif // Use unified GC header
#include "xgc_header.h"

/* ========== GC State ========== */

#define XGC_IDLE        0

/* ========== Marked Field Access ========== */

#define xr_gc_getmarked(o)      ((o)->marked)
#define xr_gc_setmarked(o, m)   ((o)->marked = (uint8_t)(m))

/*
 * Color bit definitions are ONLY in xcoro_gc.h (dual-white).
 * Global GC manages fixedgc objects that live forever and never
 * participate in mark-sweep, so they don't need color marking.
 */

/* ========== GC Main Structure ========== */

struct XrayIsolate;
typedef struct XrGC XrGC;
typedef struct XrCoroGC XrCoroGC;

// Maximum GC type ID
#define XGC_MAX_TYPES   64

/* ========== Type Function Types (must be before XrGC) ========== */

struct XrGC;  // Forward declaration
typedef void (*XrGCDestroyFn)(XrGCHeader *obj, XrCoroGC *owning_gc);
typedef struct XrGCHeader** (*XrGCGetGCListFn)(XrGCHeader *obj);

typedef struct XrGC {
    uint8_t gcstate;
    uint8_t _pad[7];
    struct XrayIsolate *isolate;
    int64_t totalbytes;
    XrGCHeader *fixedgc;      // Fixed objects (compile-time)
    size_t object_count;
} XrGC;

/* ========== Core API ========== */

XR_FUNC void xr_gc_init(XrGC *gc, struct XrayIsolate *isolate);
XR_FUNC void xr_gc_cleanup(XrGC *gc);
XR_FUNC void* xr_gc_alloc(XrGC *gc, size_t size, uint8_t type);
XR_FUNC XrGCHeader* xr_gc_newobj(XrGC *gc, uint8_t type, size_t size);

/* ========== Compile-Time Type Function Tables ========== */

extern const XrGCDestroyFn g_destroy_funcs[XGC_MAX_TYPES];

// Destroy functions (non-static, referenced by const tables)
XR_FUNC void xr_gc_destroy_array(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_map(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_set(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_stringbuilder(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_channel(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_coroutine(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void regex_object_destroy(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_logger(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_json(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_task(XrGCHeader *obj, XrCoroGC *owning_gc);

/* ========== Debug API ========== */

XR_FUNC void xr_gc_printstats(XrGC *gc);

#endif // XGC_INTERNAL_H
