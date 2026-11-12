/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_string.h - Closed String binding operations on actual owners
 */
#ifndef XLIR_STRING_H
#define XLIR_STRING_H
#include "xlir_builder.h"
typedef enum XrLirStringOperation {
    XR_LIR_STRING_OP_APPEND_OWNED = 512,
    XR_LIR_STRING_OP_APPEND_CELL,
    XR_LIR_STRING_OP_APPEND_STATE,
    XR_LIR_STRING_OP_BORROW_CELL,
    XR_LIR_STRING_OP_BORROW_STATE,
    XR_LIR_STRING_OP_READ_REF,
    XR_LIR_STRING_OP_WRITE_REF,
    XR_LIR_STRING_OP_APPEND_REF,
    XR_LIR_STRING_OP_END
} XrLirStringOperation;
XR_FUNC bool xr_lir_string_intrinsic_valid(const XrLirIntrinsic *);
XR_FUNC uint32_t xr_lir_string_role(const XrLirModule *, const XrLirInstructionView *);
XR_FUNC XrLirStatus xr_lir_string_operation(const XrLirModule *, XrLirStringOperation, uint32_t *);
XR_FUNC XrLirStatus xr_lir_string_catalog_install(XrLirModule *, bool views, bool arrays, bool display);
XR_FUNC const char *xr_lir_string_native_symbol(XrLirStringOperation);
#endif // XLIR_STRING_H
