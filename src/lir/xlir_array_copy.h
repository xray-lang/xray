/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array_copy.h - Receiving-owner copies of closed Array metadata
 */
#ifndef XLIR_ARRAY_COPY_H
#define XLIR_ARRAY_COPY_H
#include "xlir_array.h"

static inline XrLirStatus lir_copy_array_registry(const XrLirModule *input, XrLirModule *output) {
    for (uint32_t i = 0; i < xr_lir_intrinsic_count(input); ++i) {
        XrLirIntrinsic entry = {0};
        XrLirArrayIntrinsic shape = {0};
        XrLirStatus status = xr_lir_intrinsic_at(input, i, &entry);
        if (status == XR_LIR_OK) status = xr_lir_array_intrinsic_view(input, entry.id, &shape);
        if (status == XR_LIR_OK && shape.id) status = xr_lir_set_array_intrinsic(output, &shape);
        if (status != XR_LIR_OK) return status;
    }
    return XR_LIR_OK;
}

static inline XrLirStatus lir_copy_array_signature(const XrLirModule *input,
    XrLirFunction source, XrLirFunction target) {
    XrLirArrayShape shape = {0};
    XrLirStatus status = xr_lir_array_result_view(input, source, &shape);
    if (status == XR_LIR_OK && shape.element)
        status = xr_lir_set_array_result((XrLirModule *)target.module, target, &shape);
    XrLirFunctionView view = {0};
    if (status == XR_LIR_OK) status = xr_lir_function_view(input, source, &view);
    for (uint32_t p = 0; status == XR_LIR_OK && p < view.parameter_count; ++p) {
        if (view.parameter_types[p] != XR_LIR_ARRAY && view.parameter_types[p] != XR_LIR_ARRAY_CELL &&
            view.parameter_types[p] != XR_LIR_ARRAY_REF) continue;
        XrLirValue value = {0};
        status = xr_lir_function_parameter((XrLirModule *)target.module, target, p, &value);
        if (status == XR_LIR_OK) status = xr_lir_array_shape_view(input,
            (XrLirValue){input, source.id, view.parameters[p]}, &shape);
        if (status == XR_LIR_OK) status = xr_lir_set_array_shape((XrLirModule *)target.module, value, &shape);
    }
    return status;
}

static inline XrLirStatus lir_copy_array_value(const XrLirModule *input,
    XrLirValue source, XrLirValue target) {
    XrLirType type = XR_LIR_TYPE_INVALID;
    XrLirStatus status = xr_lir_value_type(input, source, &type);
    if (status != XR_LIR_OK || (type != XR_LIR_ARRAY && type != XR_LIR_ARRAY_CELL &&
        type != XR_LIR_ARRAY_REF)) return status;
    XrLirArrayShape shape = {0};
    status = xr_lir_array_shape_view(input, source, &shape);
    if (status == XR_LIR_OK)
        status = xr_lir_set_array_shape((XrLirModule *)target.module, target, &shape);
    return status;
}
#endif // XLIR_ARRAY_COPY_H
