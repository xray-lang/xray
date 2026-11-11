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
#include "xlir.h"

typedef struct XrLirArrayShape {
    XrLirType element;
} XrLirArrayShape;

typedef struct XrLirArrayIntrinsic {
    uint32_t id;
    const XrLirType *parameter_elements;
    uint32_t parameter_count;
    XrLirType result_element;
    /* The sole scalar/String parameter repeats, including zero operands.
     * Only an Array constructor may request this typed variadic shape. */
    bool variadic;
} XrLirArrayIntrinsic;

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
