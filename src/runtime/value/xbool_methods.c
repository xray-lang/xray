/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbool_methods.c - Bool method dispatch table.
 *
 * The actual method body lives as `static inline` in xbool_methods.h
 * so AOT-generated C inlines it at the call site. Taking its address
 * here forces the compiler to emit a single out-of-line copy that the
 * VM interpreter reaches through xr_bool_method_table[symbol].fn.
 */

#include "xbool_methods.h"
#include "../object/xnative_type.h"

void xr_bool_register_native_type(XrayIsolate *isolate) {
    static const XrNativeMethod bool_methods[] = {
        {"toString", xr_bool_to_string, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo bool_info = {
        .name = "Bool",
        .gc_type = XR_TBOOL,
        .methods = bool_methods,
        .getters = NULL,
        .static_methods = NULL,
    };
    xr_register_native_type(isolate, &bool_info);
}
