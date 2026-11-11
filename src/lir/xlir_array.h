/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array.h - Closed element shapes owned by the receiving SSA module
 *
 * KEY CONCEPT:
 *   An Array is an owned logical value. Its exact element shape survives calls,
 *   cells and selected edges without borrowing a source type-pool pointer.
 */
#ifndef XLIR_ARRAY_H
#define XLIR_ARRAY_H
#include "xlir_builder.h"

typedef struct XrLirArrayShape {
    XrLirType element;
} XrLirArrayShape;

typedef enum XrLirArrayOperation {
    XR_LIR_ARRAY_OP_INVALID,
    XR_LIR_ARRAY_OP_NEW, XR_LIR_ARRAY_OP_LEN, XR_LIR_ARRAY_OP_GET,
    XR_LIR_ARRAY_OP_SET, XR_LIR_ARRAY_OP_PUSH
} XrLirArrayOperation;

typedef struct XrLirArrayIntrinsic {
    uint32_t id;
    const XrLirType *parameter_elements;
    uint32_t parameter_count;
    XrLirType result_element;
    /* The sole scalar/String parameter repeats, including zero operands.
     * Only an Array constructor may request this typed variadic shape. */
    bool variadic;
    XrLirArrayOperation operation;
} XrLirArrayIntrinsic;

/* Creation owns both closed element families and copies any extra registry.
 * Operation IDs are module-local. Query them before emitting calls instead of
 * assigning numeric meanings in a lowering or backend. Output must be empty. */
XR_FUNC XrLirStatus xr_lir_array_module_create(const XrLirRegistry *, XrLirModule **);
XR_FUNC XrLirStatus xr_lir_array_operation(const XrLirModule *, XrLirType element,
    XrLirArrayOperation, uint32_t *id);

/* Explicit shapes belong only to function and block parameters. Instruction
 * results derive their shape from the actual producer, callee or intrinsic.
 * The first closed element families are I64 and owned String. */
XR_FUNC XrLirStatus xr_lir_set_array_shape(XrLirModule *, XrLirValue,
    const XrLirArrayShape *);
XR_FUNC XrLirStatus xr_lir_set_array_result(XrLirModule *, XrLirFunction,
    const XrLirArrayShape *);
/* The module copies this complete parameter vector. Non-Array parameters and
 * results use INVALID. No descriptor can reinterpret an unrelated scalar. */
XR_FUNC XrLirStatus xr_lir_set_array_intrinsic(XrLirModule *, const XrLirArrayIntrinsic *);
XR_FUNC XrLirStatus xr_lir_array_shape_view(const XrLirModule *, XrLirValue,
    XrLirArrayShape *);
XR_FUNC XrLirStatus xr_lir_array_result_view(const XrLirModule *, XrLirFunction,
    XrLirArrayShape *);
XR_FUNC XrLirStatus xr_lir_array_intrinsic_view(const XrLirModule *, uint32_t,
    XrLirArrayIntrinsic *);

#endif // XLIR_ARRAY_H
