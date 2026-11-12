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
    XR_LIR_ARRAY_OP_SET, XR_LIR_ARRAY_OP_PUSH, XR_LIR_ARRAY_OP_REPEAT
} XrLirArrayOperation;

typedef struct XrLirArrayIntrinsic {
    uint32_t id;
    const XrLirType *parameter_elements;
    uint32_t parameter_count;
    XrLirType result_element;
    /* NEW takes a typed variadic list, including zero operands. REPEAT takes
     * exactly length:I64 and a borrowed fill of the result's element type. */
    bool variadic;
    XrLirArrayOperation operation;
} XrLirArrayIntrinsic;

/* Creation owns the closed element catalog and copies any extra registry.
 * Operation IDs are module-local. Query them before emitting calls instead of
 * assigning numeric meanings in a lowering or backend. Output must be empty. */
XR_FUNC XrLirStatus xr_lir_array_module_create(const XrLirRegistry *, XrLirModule **);
/* Install the same closed catalog in an existing receiving owner before
 * function declarations. The module must have no intrinsic registry yet. */
XR_FUNC XrLirStatus xr_lir_array_catalog_install(XrLirModule *);
XR_FUNC XrLirStatus xr_lir_array_operation(const XrLirModule *, XrLirType element,
    XrLirArrayOperation, uint32_t *id);
/* SET and PUSH have exact ARRAY_CELL and ARRAY_REF receiver signatures.
 * The operation query above selects ARRAY_CELL. This query selects either
 * mutable binding representation without reading or copying its current value. */
XR_FUNC XrLirStatus xr_lir_array_mutator(const XrLirModule *, XrLirType element,
    XrLirType receiver, XrLirArrayOperation, uint32_t *id);

/* Explicit shapes belong only to function and block parameters. Instruction
 * results derive their shape from the actual producer, callee or intrinsic.
 * ARRAY_REF function parameters carry the same exact element shape. BORROW
 * derives it from a live ARRAY_CELL or a mutable Array STATE_REF in this owner.
 * READ creates an owned Array snapshot. WRITE replaces the referenced binding.
 * Borrowing reads no payload or readiness flags, and neither owns nor releases
 * the underlying Cell or state. References cannot cross edges or escape calls.
 * Closed elements are I64, owned String, U8 and U64. Their shape never implies
 * that a backend implements that family or its scalar operations. */
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
