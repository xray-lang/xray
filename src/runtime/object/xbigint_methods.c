/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbigint_methods.c - BigInt method dispatch table.
 *
 * Method bodies are `static inline` in xbigint_methods.h so AOT
 * inlines them at the call site. Taking each address here forces
 * the compiler to emit one out-of-line copy that the VM dispatcher
 * reaches through xr_bigint_method_table[symbol].fn.
 */

#include "xbigint_methods.h"
#include "xnative_type.h"

void xr_bigint_register_native_type(XrayIsolate *isolate) {
    static const XrNativeMethod bigint_methods[] = {
        {"toString", xr_bigint_to_string_method, 0},
        {"abs", xr_bigint_abs_method, 0},
        {"sign", xr_bigint_sign_method, 0},
        {"isZero", xr_bigint_is_zero_method, 0},
        {"isNegative", xr_bigint_is_negative_method, 0},
        {"isPositive", xr_bigint_is_positive_method, 0},
        {"toInt", xr_bigint_to_int_method, 0},
        {"toFloat", xr_bigint_to_float_method, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo bigint_info = {
        .name = "BigInt",
        .gc_type = XR_TBIGINT,
        .methods = bigint_methods,
        .getters = NULL,
        .static_methods = NULL,
    };
    xr_register_native_type(isolate, &bigint_info);
}
