/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfloat_methods.c - Float method dispatch table.
 */

#include "xfloat_methods.h"

const XrMethodSlot xr_float_method_table[SYMBOL_BUILTIN_COUNT] = {
    [SYMBOL_TOSTRING] =
        {
            .fn = xr_float_to_string_method,
            .min_args = 0,
            .max_args = 0,
            .flags = XR_METHOD_FLAG_MAY_THROW, /* allocates string */
        },
    [SYMBOL_TOFIXED] =
        {
            .fn = xr_float_to_fixed_method,
            .min_args = 0,
            .max_args = 1,
            .flags = XR_METHOD_FLAG_MAY_THROW,
        },
    [SYMBOL_FLOOR] =
        {
            .fn = xr_float_floor_method,
            .min_args = 0,
            .max_args = 0,
            .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
        },
    [SYMBOL_CEIL] =
        {
            .fn = xr_float_ceil_method,
            .min_args = 0,
            .max_args = 0,
            .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
        },
    [SYMBOL_ROUND] =
        {
            .fn = xr_float_round_method,
            .min_args = 0,
            .max_args = 0,
            .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
        },
    [SYMBOL_ABS] =
        {
            .fn = xr_float_abs_method,
            .min_args = 0,
            .max_args = 0,
            .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
        },
    [SYMBOL_SQRT] =
        {
            .fn = xr_float_sqrt_method,
            .min_args = 0,
            .max_args = 0,
            .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
        },
    [SYMBOL_TOINT] =
        {
            .fn = xr_float_to_int_method,
            .min_args = 0,
            .max_args = 0,
            .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
        },
    [SYMBOL_POW] =
        {
            .fn = xr_float_pow_method,
            .min_args = 0,
            .max_args = 1,
            .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
        },
};

/* ========== XrClass Registration ========== */

#include "../object/xnative_type.h"

void xr_float_register_native_type(XrayIsolate *isolate) {
    static const XrNativeMethod float_methods[] = {
        {"toString", xr_float_to_string_method, 0},
        {"toFixed", xr_float_to_fixed_method, 0},
        {"floor", xr_float_floor_method, 0},
        {"ceil", xr_float_ceil_method, 0},
        {"round", xr_float_round_method, 0},
        {"abs", xr_float_abs_method, 0},
        {"sqrt", xr_float_sqrt_method, 0},
        {"toInt", xr_float_to_int_method, 0},
        {"pow", xr_float_pow_method, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo float_info = {
        .name = "Float",
        .gc_type = XR_TFLOAT,
        .methods = float_methods,
        .getters = NULL,
        .static_methods = NULL,
    };
    xr_register_native_type(isolate, &float_info);
}
