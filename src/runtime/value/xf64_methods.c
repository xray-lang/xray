/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xf64_methods.c - f64 method dispatch table.
 */

#include "xf64_methods.h"
#include "../object/xnative_type.h"

void xr_f64_register_native_type(XrVMRuntime *isolate) {
    static const XrNativeMethod f64_methods[] = {
        {"toString", xr_f64_to_string_method, 0},
        {"toFixed", xr_f64_to_fixed_method, 0},
        {"floor", xr_f64_floor_method, 0},
        {"ceil", xr_f64_ceil_method, 0},
        {"round", xr_f64_round_method, 0},
        {"abs", xr_f64_abs_method, 0},
        {"sqrt", xr_f64_sqrt_method, 0},
        {"isNaN", xr_f64_is_nan_method, 0},
        {"toI64", xr_f64_to_int_method, 0},
        {"pow", xr_f64_pow_method, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo f64_info = {
        .name = "f64",
        .gc_type = XR_TFLOAT,
        .methods = f64_methods,
        .getters = NULL,
        .static_methods = NULL,
    };
    xr_register_native_type(isolate, &f64_info);
}
