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
#include "xlir_types.h"
#include "xlir_callable.h"
#include "xlir_cell.h"

static inline bool lir_builtin_signature(XrLirType result, const XrLirType *parameters,
    uint32_t count) {
    if ((uint32_t)result >= XR_LIR_TYPE_COUNT) return false;
    for (uint32_t p = 0; p < count; ++p)
        if ((uint32_t)parameters[p] >= XR_LIR_TYPE_COUNT) return false;
    return true;
}

static inline bool lir_builtin_or_fixed_type(const XrLirModule *module, XrLirType type) {
    if (type < XR_LIR_TYPE_COUNT) return true;
    XrLirTypeView view = {0};
    XrLirTypeLayout layout;
    return xr_lir_type_view(module, type, &view) == XR_LIR_OK &&
        (view.kind == XR_LIR_TYPE_FIXED_ARRAY || view.kind == XR_LIR_TYPE_NULLABLE || view.kind == XR_LIR_TYPE_TUPLE) &&
        xr_lir_type_layout(module, type, &layout) == XR_LIR_OK && !layout.managed;
}

static inline bool lir_definition_type(const XrLirModule *module, XrLirType type) {
    if (lir_builtin_or_fixed_type(module, type)) return true;
    XrLirCallableType signature;
    XrLirCellType cell;
    if (xr_lir_cell_type_view(module, type, &cell) == XR_LIR_OK) return cell.payload == XR_LIR_I64;
    return xr_lir_callable_type_view(module, type, &signature) == XR_LIR_OK;
}

static inline bool lir_definition_cell_captures(const XrLirModule *module,
    XrLirFunction function, const XrLirFunctionView *view) {
    XrLirClosureTarget closure = {0};
    XrLirCellType cell;
    if (xr_lir_cell_type_view(module, view->result, &cell) == XR_LIR_OK) return false;
    xr_lir_closure_target_view(module, function, &closure);
    for (uint32_t p = 0; p < view->parameter_count; ++p)
        if (xr_lir_cell_type_view(module, view->parameter_types[p], &cell) == XR_LIR_OK &&
            (!closure.callable || p >= closure.capture_count)) return false;
    return true;
}

/* Definitions and State use the owned Fixed projection. Intrinsic and external
 * boundaries retain their closed builtin ABI until a typed consumer exists. */
static inline bool lir_module_has_builtin_abi(const XrLirModule *module) {
    if (!xr_lir_module_is_readable(module)) return false;
    if (!xr_lir_constructed_type_count(module)) return true;
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
            !lir_definition_type(module, view.result) ||
            !lir_definition_cell_captures(module, function, &view)) return false;
        for (uint32_t p = 0; p < view.parameter_count; ++p)
            if (!lir_definition_type(module, view.parameter_types[p])) return false;
        if (view.kind != XR_LIR_FUNCTION_DEFINITION &&
            !lir_builtin_signature(view.result, view.parameter_types, view.parameter_count)) return false;
        if (view.result == XR_LIR_ARRAY) {
            XrLirArrayShape shape;
            if (xr_lir_array_result_view(module, function, &shape) != XR_LIR_OK ||
                (uint32_t)shape.element >= XR_LIR_TYPE_COUNT) return false;
        }
        for (uint32_t value = 0; value < view.value_count; ++value) {
            XrLirValue handle = {module, function.id, value + 1};
            XrLirType type;
            if (xr_lir_value_type(module, handle, &type) != XR_LIR_OK) return false;
            if (type == XR_LIR_STATE_REF) {
                XrLirStateReference shape;
                if (xr_lir_state_reference_view(module, handle, &shape) != XR_LIR_OK ||
                    !lir_definition_type(module, shape.payload) ||
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
        if (!lir_definition_type(module, layout.slots[i].payload) ||
            (uint32_t)layout.slots[i].array_element >= XR_LIR_TYPE_COUNT) return false;
    return true;
}
#endif // XLIR_BUILTIN_ADMISSION_H
