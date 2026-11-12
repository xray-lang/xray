/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_builtin_admission.h - Closed physical type domains for builtin backends
 */
#ifndef XLIR_BUILTIN_ADMISSION_H
#define XLIR_BUILTIN_ADMISSION_H
#include "xlir_array.h"
#include "xlir_state.h"

static inline bool lir_builtin_signature(XrLirType result, const XrLirType *parameters,
    uint32_t count) {
    if ((uint32_t)result >= XR_LIR_TYPE_COUNT) return false;
    for (uint32_t p = 0; p < count; ++p)
        if ((uint32_t)parameters[p] >= XR_LIR_TYPE_COUNT) return false;
    return true;
}

/* An unused signature or module slot still belongs to the received artifact.
 * A constructed ordinal must never reach a builtin-only layout or emitter. */
static inline bool lir_module_has_builtin_types(const XrLirModule *module) {
    for (uint32_t i = 0; i < xr_lir_intrinsic_count(module); ++i) {
        XrLirIntrinsic signature;
        if (xr_lir_intrinsic_at(module, i, &signature) != XR_LIR_OK ||
            !lir_builtin_signature(signature.result, signature.parameters, signature.parameter_count))
            return false;
    }
    for (uint32_t i = 0; i < xr_lir_function_count(module); ++i) {
        XrLirFunction function;
        XrLirFunctionView view;
        if (xr_lir_function_at(module, i, &function) != XR_LIR_OK ||
            xr_lir_function_view(module, function, &view) != XR_LIR_OK ||
            !lir_builtin_signature(view.result, view.parameter_types, view.parameter_count))
            return false;
        if (view.result == XR_LIR_ARRAY) {
            XrLirArrayShape shape;
            if (xr_lir_array_result_view(module, function, &shape) != XR_LIR_OK ||
                (uint32_t)shape.element >= XR_LIR_TYPE_COUNT) return false;
        }
        for (uint32_t value = 0; value < view.value_count; ++value) {
            XrLirValue handle = {module, function.id, value + 1};
            XrLirType type;
            if (xr_lir_value_type(module, handle, &type) != XR_LIR_OK ||
                (uint32_t)type >= XR_LIR_TYPE_COUNT) return false;
            if (type == XR_LIR_STATE_REF) {
                XrLirStateReference shape;
                if (xr_lir_state_reference_view(module, handle, &shape) != XR_LIR_OK ||
                    (uint32_t)shape.payload >= XR_LIR_TYPE_COUNT ||
                    (uint32_t)shape.array_element >= XR_LIR_TYPE_COUNT) return false;
            } else if (type == XR_LIR_ARRAY || type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF) {
                XrLirArrayShape shape;
                if (xr_lir_array_shape_view(module, handle, &shape) != XR_LIR_OK ||
                    (uint32_t)shape.element >= XR_LIR_TYPE_COUNT) return false;
            }
        }
    }
    XrLirStateLayout layout;
    if (xr_lir_state_layout_view(module, &layout) != XR_LIR_OK) return false;
    for (uint32_t i = 0; i < layout.slot_count; ++i)
        if ((uint32_t)layout.slots[i].payload >= XR_LIR_TYPE_COUNT ||
            (uint32_t)layout.slots[i].array_element >= XR_LIR_TYPE_COUNT) return false;
    return true;
}
#endif // XLIR_BUILTIN_ADMISSION_H
