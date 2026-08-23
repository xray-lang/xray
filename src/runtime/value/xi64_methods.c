/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi64_methods.c - i64 method dispatch table.
 */

#include "xi64_methods.h"
#include "../object/xnative_type.h"

void xr_i64_register_native_type(XrVMRuntime *isolate) {
    static const XrNativeMethod i64_methods[] = {
        {"toString", xr_i64_to_string_method, 0},
        {"abs", xr_i64_abs_method, 0},
        {"toBigInt", xr_i64_to_bigint_method, 0},
        {"max", xr_i64_max_method, 0},
        {"min", xr_i64_min_method, 0},
        {"toF64", xr_i64_to_float_method, 0},
        {"toHex", xr_i64_to_hex_method, 0},
        {"sqrt", xr_i64_sqrt_method, 0},
        {"pow", xr_i64_pow_method, 0},
        {"checkedAdd", xr_i64_checked_add_method, 0},
        {"checkedSub", xr_i64_checked_sub_method, 0},
        {"checkedMul", xr_i64_checked_mul_method, 0},
        {"saturatingAdd", xr_i64_saturating_add_method, 0},
        {"saturatingSub", xr_i64_saturating_sub_method, 0},
        {"saturatingMul", xr_i64_saturating_mul_method, 0},
        {"wrappingAdd", xr_i64_wrapping_add_method, 0},
        {"wrappingSub", xr_i64_wrapping_sub_method, 0},
        {"wrappingMul", xr_i64_wrapping_mul_method, 0},
        {"addOverflows", xr_i64_add_overflows_method, 0},
        {"subOverflows", xr_i64_sub_overflows_method, 0},
        {"mulOverflows", xr_i64_mul_overflows_method, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo i64_info = {
        .name = "i64",
        .gc_type = XR_TINT,
        .methods = i64_methods,
        .getters = NULL,
        .static_methods = NULL,
    };
    xr_register_native_type(isolate, &i64_info);
}
