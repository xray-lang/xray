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

const XrMethodSlot xr_bool_method_table[SYMBOL_BUILTIN_COUNT] = {
    [SYMBOL_TOSTRING] = {
        .fn       = xr_bool_to_string,
        .min_args = 0,
        .max_args = 0,
        .flags    = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
    },
};
