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

void xr_int_register_native_type(XrayIsolate *isolate) {
    static const XrNativeMethod int_methods[] = {
        {"toString", xr_int_to_string_method, 0},
        {"abs", xr_int_abs_method, 0},
        {"toBigInt", xr_int_to_bigint_method, 0},
        {"max", xr_int_max_method, 0},
        {"min", xr_int_min_method, 0},
        {"toFloat", xr_int_to_float_method, 0},
        {"toHex", xr_int_to_hex_method, 0},
        {"pow", xr_int_pow_method, 0},
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
