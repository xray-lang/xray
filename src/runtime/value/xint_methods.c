/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xint_methods.c - Int method dispatch table.
 */

#include "xint_methods.h"
#include "../object/xnative_type.h"

void xr_int_register_native_type(XrVMRuntime *isolate) {
    static const XrNativeMethod int_methods[] = {
        {"toString", xr_int_to_string_method, 0},
        {"abs", xr_int_abs_method, 0},
        {"toBigInt", xr_int_to_bigint_method, 0},
        {"max", xr_int_max_method, 0},
        {"min", xr_int_min_method, 0},
        {"toFloat", xr_int_to_float_method, 0},
        {"toHex", xr_int_to_hex_method, 0},
        {"pow", xr_int_pow_method, 0},
        {"checkedAdd", xr_int_checked_add_method, 0},
        {"checkedSub", xr_int_checked_sub_method, 0},
        {"checkedMul", xr_int_checked_mul_method, 0},
        {"saturatingAdd", xr_int_saturating_add_method, 0},
        {"saturatingSub", xr_int_saturating_sub_method, 0},
        {"saturatingMul", xr_int_saturating_mul_method, 0},
        {"wrappingAdd", xr_int_wrapping_add_method, 0},
        {"wrappingSub", xr_int_wrapping_sub_method, 0},
        {"wrappingMul", xr_int_wrapping_mul_method, 0},
        {"popcount", xr_int_popcount_method, 0},
        {"leadingZeros", xr_int_leading_zeros_method, 0},
        {"trailingZeros", xr_int_trailing_zeros_method, 0},
        {"byteswap", xr_int_byteswap_method, 0},
        {"rotateLeft", xr_int_rotate_left_method, 0},
        {"rotateRight", xr_int_rotate_right_method, 0},
        {"addOverflows", xr_int_add_overflows_method, 0},
        {"subOverflows", xr_int_sub_overflows_method, 0},
        {"mulOverflows", xr_int_mul_overflows_method, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo int_info = {
        .name = "Int",
        .gc_type = XR_TINT,
        .methods = int_methods,
        .getters = NULL,
        .static_methods = NULL,
    };
    xr_register_native_type(isolate, &int_info);
}
