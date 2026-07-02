/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xobj_ops.h - Object type capability interfaces.
 */

#ifndef XOBJ_OPS_H
#define XOBJ_OPS_H

#include <stdint.h>
#include "../../base/xdefs.h"
#include "xobj_header.h"

#ifndef XR_VALUE_DEFINED
typedef struct XrValue XrValue;
#endif

struct XrCoroHeap;
struct XrVMRuntime;
struct XrCopyContext;

// Object type table capacity.
#define XR_OBJ_TYPE_MAX 64

typedef void (*XrObjDestroyFn)(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
typedef XrValue (*XrObjDeepCopyFn)(struct XrCopyContext *ctx, XrObjHeader *obj);
typedef XrValue (*XrObjToSharedFn)(struct XrVMRuntime *X, XrObjHeader *obj);
typedef struct XrObjHeader **(*XrObjGetRefListFn)(XrObjHeader *obj);

/* Per-type capability tables.
 *
 * Each capability is intentionally stored in its own table. RC/fixed heap
 * cleanup only needs destroy callbacks, while cross-coroutine transfer needs
 * deep-copy and to-shared callbacks. Keeping the tables split prevents the
 * minimal AOT runtime from pulling deep-copy/share code merely because it can
 * destroy an object.
 *
 * Extension types (registered via xr_register_extension_destroy) live in the
 * same runtime-core destroy table as built-in types.
 */

// Transfer tables: defined in xdeep_copy.c and only pulled when transfer logic is linked.
extern const XrObjDeepCopyFn xr_obj_deep_copy_ops[XR_OBJ_TYPE_MAX];
extern const XrObjToSharedFn xr_obj_to_shared_ops[XR_OBJ_TYPE_MAX];

// Destroy functions (non-static, referenced by const tables).
XR_FUNC void xr_obj_destroy_array(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_map(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_set(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_channel(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_closure(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_cell(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_coroutine(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_instance(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_task(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_work_queue(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_result_group(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_countdown_latch(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_semaphore(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
// NetConn / NetListener destroy handled by native body descriptors.

XR_FUNC bool xr_obj_type_may_need_finalize(uint8_t type);

#endif  // XOBJ_OPS_H
