/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_native_callable.h - Physical projections shared by code consumers
 */
#ifndef XLIR_NATIVE_CALLABLE_H
#define XLIR_NATIVE_CALLABLE_H
#include "xlir_callable.h"
#include "xlir_cell.h"
#include "xlir_pod.h"
#include "runtime/native/xnative_callable.h"

static inline bool lir_native_callable_value(XrLirType type, XrLirType element,
    XrtCallableValueSpec *output) {
    XrPodScalar scalar = lir_pod_scalar(type);
    if (scalar < XR_POD_SCALAR_COUNT) *output = (XrtCallableValueSpec){XRT_CALLABLE_SCALAR, scalar};
    else if (type == XR_LIR_STRING) *output = (XrtCallableValueSpec){XRT_CALLABLE_STRING, 0};
    else if (type == XR_LIR_MAP_STRING_I64)
        *output = (XrtCallableValueSpec){XRT_CALLABLE_MAP_STRING_I64, 0};
    else if (type == XR_LIR_ARRAY) {
        static const XrLirType elements[] = {XR_LIR_I64, XR_LIR_STRING, XR_LIR_U8, XR_LIR_U64};
        uint32_t family = 0;
        while (family < 4 && elements[family] != element) ++family;
        if (family == 4) return false;
        *output = (XrtCallableValueSpec){XRT_CALLABLE_ARRAY, family};
    } else if (type >= XR_LIR_TYPE_COUNT)
        *output = (XrtCallableValueSpec){XRT_CALLABLE_CONSTRUCTED, type - XR_LIR_TYPE_COUNT};
    else return false;
    return true;
}

/* The caller supplies owned workspace from its existing allocator. Type IDs
 * preserve the receiving pool order; these rows contain no Source pointers. */
static inline XrLirStatus lir_native_callable_spec(const XrLirModule *module, XrLirType type,
    XrtCallableParameterSpec *parameters, uint32_t capacity, XrtCallableTypeSpec *output) {
    XrLirTypeView view = {0};
    if (!output || xr_lir_type_view(module, type, &view) != XR_LIR_OK) return XR_LIR_INVALID_ARGUMENT;
    XrtCallableTypeSpec spec = {0};
    if (view.kind == XR_LIR_TYPE_RECORD) {
        const XrLirRecordField *fields;
        uint32_t count;
        if (xr_lir_record_fields(module, type, &fields, &count) != XR_LIR_OK ||
            count > capacity || (count && !parameters)) return XR_LIR_INVALID_ARGUMENT;
        spec.kind = XRT_CALLABLE_RECORD;
        spec.parameters = count ? parameters : NULL;
        spec.parameter_count = count;
        for (uint32_t f = 0; f < count; ++f) {
            parameters[f] = (XrtCallableParameterSpec){.name=fields[f].name, .name_length=fields[f].name_length};
            if (!lir_native_callable_value(fields[f].type, 0, &parameters[f].value)) return XR_LIR_UNSUPPORTED;
        }
    } else if (view.kind == XR_LIR_TYPE_CALLABLE) {
        XrLirCallableType signature;
        if (xr_lir_callable_type_view(module, type, &signature) != XR_LIR_OK ||
            signature.parameter_count > capacity || (signature.parameter_count && !parameters))
            return XR_LIR_INVALID_ARGUMENT;
        if (signature.throws) return XR_LIR_UNSUPPORTED;
        spec.kind = XRT_CALLABLE_SIGNATURE;
        spec.parameters = signature.parameter_count ? parameters : NULL;
        spec.parameter_count = signature.parameter_count;
        spec.flags = signature.flags;
        if (!lir_native_callable_value(signature.result, signature.result_element, &spec.result)) return XR_LIR_UNSUPPORTED;
        for (uint32_t p = 0; p < signature.parameter_count; ++p) {
            XrLirCallableParameter from = signature.parameters[p];
            parameters[p] = (XrtCallableParameterSpec){0};
            if (!lir_native_callable_value(from.type, from.array_element, &parameters[p].value)) return XR_LIR_UNSUPPORTED;
            parameters[p].mode = (uint32_t)from.mode;
            parameters[p].escaping = from.escaping;
        }
    } else {
        XrLirType element = view.element, family = 0;
        if (view.kind == XR_LIR_TYPE_FIXED_ARRAY) {
            spec.kind = XRT_CALLABLE_FIXED_BYTES;
            spec.count = view.count;
        } else if (view.kind == XR_LIR_TYPE_TUPLE) {
            spec.kind = XRT_CALLABLE_TUPLE_I64;
            spec.count = view.count;
        } else if (view.kind == XR_LIR_TYPE_NULLABLE) spec.kind = XRT_CALLABLE_NULLABLE;
        else if (view.kind == XR_LIR_TYPE_CELL) {
            XrLirCellType cell;
            if (xr_lir_cell_type_view(module, type, &cell) != XR_LIR_OK) return XR_LIR_INVALID_ARGUMENT;
            spec.kind = XRT_CALLABLE_CELL;
            element = cell.payload;
            family = cell.array_element;
        } else return XR_LIR_UNSUPPORTED;
        if (!lir_native_callable_value(element, family, &spec.element)) return XR_LIR_UNSUPPORTED;
    }
    *output = spec;
    return XR_LIR_OK;
}
#endif // XLIR_NATIVE_CALLABLE_H
