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
#include "../../os/os_thread.h"

#ifndef XR_VALUE_DEFINED
typedef struct XrValue XrValue;
#endif

// XR_THREAD_LOCAL is provided by base/xdefs.h via xgc_header.h.
#include "xgc_header.h"

/* ========== GC State ========== */

#define XGC_IDLE 0

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
#define XGC_MAX_TYPES 64

/* ========== Type Function Types (must be before XrGC) ========== */

struct XrGC;  // Forward declaration
struct XrayIsolate;
struct XrCopyContext;

typedef void (*XrGCDestroyFn)(XrGCHeader *obj, XrCoroGC *owning_gc);
typedef XrValue (*XrGCDeepCopyFn)(struct XrCopyContext *ctx, XrGCHeader *obj);
typedef XrValue (*XrGCToSharedFn)(struct XrayIsolate *X, XrGCHeader *obj);
typedef struct XrGCHeader **(*XrGCGetGCListFn)(XrGCHeader *obj);

typedef struct XrGCObjectNode {
    XrGCHeader *obj;
    struct XrGCObjectNode *next;
} XrGCObjectNode;

/* ========== Per-Type Capability Tables ==========
 *
 * Each capability is intentionally stored in its own table. RC/fixedgc cleanup
 * only needs destroy callbacks, while cross-coroutine transfer needs deep-copy
 * and to-shared callbacks. Keeping the tables split prevents the minimal AOT
 * runtime from pulling deep-copy/share code merely because it can destroy an
 * object.
 *
 * Extension types (registered via xr_register_extension_destroy) live in
 * per-isolate/runtime-core tables and are consulted as a fallback when the
 * compile-time destroy slot is empty. */

typedef struct XrGC {
    uint8_t gcstate;
    uint8_t _pad[7];
    struct XrayIsolate *isolate;
    int64_t totalbytes;
    XrGCObjectNode *fixedgc;  // Fixed objects (compile-time)
    size_t object_count;
} XrGC;

/* ========== Core API ========== */

XR_FUNC void xr_gc_init(XrGC *gc, struct XrayIsolate *isolate);
XR_FUNC void xr_gc_cleanup(XrGC *gc);
XR_FUNC void *xr_gc_alloc(XrGC *gc, size_t size, uint8_t type);
XR_FUNC XrGCHeader *xr_gc_newobj(XrGC *gc, uint8_t type, size_t size);
XR_FUNC bool xr_gc_type_may_need_finalize(uint8_t type);

/* Compile-time RC dup/drop primitives are inline in xcoro_gc.h
 * (xr_rc_retain_value / xr_rc_release_value): one shared implementation
 * for the VM and container runtime. */

/* ========== Compile-Time Type Function Tables ========== */

// Destroy table: defined in xgc.c, referenced by RC/fixedgc cleanup.
extern const XrGCDestroyFn g_type_destroy_ops[XGC_MAX_TYPES];

// Transfer tables: defined in xdeep_copy.c and only pulled when transfer
// logic is linked.
extern const XrGCDeepCopyFn g_type_deep_copy_ops[XGC_MAX_TYPES];
extern const XrGCToSharedFn g_type_to_shared_ops[XGC_MAX_TYPES];

// Destroy functions (non-static, referenced by const tables)
XR_FUNC void xr_gc_destroy_array(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_map(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_set(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_channel(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_closure(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_cell(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_coroutine(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_instance(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_task(XrGCHeader *obj, XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_result_group(XrGCHeader *obj, XrCoroGC *owning_gc);
// NetConn / NetListener destroy handled by native body descriptors.

/* ========== Debug API ========== */

XR_FUNC void xr_gc_printstats(XrGC *gc);

#endif  // XGC_INTERNAL_H
