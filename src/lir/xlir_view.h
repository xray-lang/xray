/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_view.h - Closed library roles on actual typed activation resources
 */
#ifndef XLIR_VIEW_H
#define XLIR_VIEW_H
#include "xlir_builder.h"

typedef enum XrLirViewOperation {
    XR_LIR_VIEW_OP_NONE,
#define XR_LIR_VIEW_OP(name, symbol, result, count, a, b, c, d, e, f) XR_LIR_VIEW_OP_##name,
#include "xlir_view_ops.def"
#undef XR_LIR_VIEW_OP
    XR_LIR_VIEW_OP_COUNT
} XrLirViewOperation;

/* The receiving registry owns signatures and numeric roles. Activation slot
 * constants remain real call operands and are checked against their producer. */
XR_FUNC XrLirStatus xr_lir_view_catalog_install(XrLirModule *);
XR_FUNC XrLirStatus xr_lir_view_operation(const XrLirModule *, XrLirViewOperation, uint32_t *);
XR_FUNC const char *xr_lir_view_native_symbol(XrLirViewOperation);
#endif // XLIR_VIEW_H
