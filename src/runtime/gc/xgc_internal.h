/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xgc_internal.h - Fixed heap and object lifecycle interfaces
 *
 * KEY CONCEPT:
 *   - Runtime objects allocated in coroutine heaps (xcoro_heap.c)
 *   - This file manages: fixed heap list, type function registration
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

/* ========== Fixed Heap State ========== */

#define XFIXED_HEAP_IDLE 0

/*
 * Color bit definitions are ONLY in xcoro_heap.h (dual-white).
 * The fixed heap manages objects that live until runtime teardown and never
 * participate in mark-sweep, so they don't need color marking.
 */

/* ========== Fixed Heap Main Structure ========== */

struct XrayIsolate;
typedef struct XrFixedHeap XrFixedHeap;
typedef struct XrCoroHeap XrCoroHeap;

// Object type table capacity
#define XR_OBJ_TYPE_MAX 64

/* ========== Object Type Capability Function Types ========== */

struct XrFixedHeap;
struct XrayIsolate;
struct XrCopyContext;

typedef void (*XrObjDestroyFn)(XrObjHeader *obj, XrCoroHeap *owner_heap);
typedef XrValue (*XrObjDeepCopyFn)(struct XrCopyContext *ctx, XrObjHeader *obj);
typedef XrValue (*XrObjToSharedFn)(struct XrayIsolate *X, XrObjHeader *obj);
typedef struct XrObjHeader **(*XrObjGetRefListFn)(XrObjHeader *obj);

typedef struct XrFixedHeapObjectNode {
    XrObjHeader *obj;
    struct XrFixedHeapObjectNode *next;
} XrFixedHeapObjectNode;

/* ========== Per-Type Capability Tables ==========
 *
 * Each capability is intentionally stored in its own table. RC/fixed heap cleanup
 * only needs destroy callbacks, while cross-coroutine transfer needs deep-copy
 * and to-shared callbacks. Keeping the tables split prevents the minimal AOT
 * runtime from pulling deep-copy/share code merely because it can destroy an
 * object.
 *
 * Extension types (registered via xr_register_extension_destroy) live in the
 * same runtime-core destroy table as built-in types. */

typedef struct XrFixedHeap {
    uint8_t state;
    uint8_t _pad[7];
    struct XrayIsolate *isolate;
    int64_t totalbytes;
    XrFixedHeapObjectNode *objects;  // Fixed-lifetime objects
    size_t object_count;
} XrFixedHeap;

/* ========== Core API ========== */

XR_FUNC void xr_fixed_heap_init(XrFixedHeap *heap, struct XrayIsolate *isolate);
XR_FUNC void xr_fixed_heap_cleanup(XrFixedHeap *heap);
XR_FUNC void *xr_fixed_heap_alloc(XrFixedHeap *heap, size_t size, uint8_t type);
XR_FUNC XrObjHeader *xr_fixed_heap_new_obj(XrFixedHeap *heap, uint8_t type, size_t size);
XR_FUNC bool xr_obj_type_may_need_finalize(uint8_t type);

/* Compile-time RC dup/drop primitives are inline in xcoro_heap.h
 * (xr_rc_retain_value / xr_rc_release_value): one shared implementation
 * for the VM and container runtime. */

/* ========== Object Type Transfer Tables ========== */

// Transfer tables: defined in xdeep_copy.c and only pulled when transfer
// logic is linked.
extern const XrObjDeepCopyFn xr_obj_deep_copy_ops[XR_OBJ_TYPE_MAX];
extern const XrObjToSharedFn xr_obj_to_shared_ops[XR_OBJ_TYPE_MAX];

// Destroy functions (non-static, referenced by const tables)
XR_FUNC void xr_obj_destroy_array(XrObjHeader *obj, XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_map(XrObjHeader *obj, XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_set(XrObjHeader *obj, XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_channel(XrObjHeader *obj, XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_closure(XrObjHeader *obj, XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_cell(XrObjHeader *obj, XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_coroutine(XrObjHeader *obj, XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_instance(XrObjHeader *obj, XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_task(XrObjHeader *obj, XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_work_queue(XrObjHeader *obj, XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_result_group(XrObjHeader *obj, XrCoroHeap *owner_heap);
// NetConn / NetListener destroy handled by native body descriptors.

/* ========== Debug API ========== */

XR_FUNC void xr_fixed_heap_print_stats(XrFixedHeap *heap);

#endif  // XGC_INTERNAL_H
