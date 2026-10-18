/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_view_operation_internal.h - Common VM and C11 view operations
 *
 * KEY CONCEPT:
 *   Constructors and consumption use real frame-owned Value scratch slots.
 */
#ifndef XXIR_CALL_VIEW_OPERATION_INTERNAL_H
#define XXIR_CALL_VIEW_OPERATION_INTERNAL_H
#include "xxir_call_view_internal.h"
#include "xxir_call_move_internal.h"
#include "xxir.h"

typedef struct XrXirCallViewOperation {
    XrXirOp operation;
    XrXirType type;
    const XrXirValue *root,*element;
    XrXirValue *owned;
    int64_t start,end;
    bool to_end;
} XrXirCallViewOperation;

/* An operation never accepts a permission bit or a caller-provided authority.
 * NEW requires a real owned Cell slot; END consumes a real owned Value slot.
 * Root/path/NLL and typed transfer are separate mandatory compiler checks. */
XR_FUNC XrXirValueStatus xr_xir_call_view_operation(XrXirCallView *view,
    const XrXirCallViewOperation *request,XrXirValue *output,XrXirFaultDetail *fault);
XR_FUNC XrXirAction xr_xir_call_view_operation_fault(XrXirValueStatus status,XrXirFaultDetail fault);
#endif // XXIR_CALL_VIEW_OPERATION_INTERNAL_H
