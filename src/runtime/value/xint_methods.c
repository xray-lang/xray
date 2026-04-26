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

const XrMethodSlot xr_int_method_table[SYMBOL_BUILTIN_COUNT] = {
    [SYMBOL_TOSTRING] = {
        .fn = xr_int_to_string_method, .min_args = 0, .max_args = 0,
        .flags = XR_METHOD_FLAG_MAY_THROW,  /* allocates */
    },
    [SYMBOL_ABS] = {
        .fn = xr_int_abs_method, .min_args = 0, .max_args = 0,
        .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
    },
    [SYMBOL_TOBIGINT] = {
        .fn = xr_int_to_bigint_method, .min_args = 0, .max_args = 0,
        .flags = 0,                          /* allocates BigInt */
    },
    [SYMBOL_MAX] = {
        .fn = xr_int_max_method, .min_args = 0, .max_args = 1,
        .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
    },
    [SYMBOL_MIN] = {
        .fn = xr_int_min_method, .min_args = 0, .max_args = 1,
        .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
    },
    [SYMBOL_TOFLOAT] = {
        .fn = xr_int_to_float_method, .min_args = 0, .max_args = 0,
        .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
    },
    [SYMBOL_TOHEX] = {
        .fn = xr_int_to_hex_method, .min_args = 0, .max_args = 0,
        .flags = XR_METHOD_FLAG_MAY_THROW,
    },
    /* Math methods: no-op for integer receiver, no float roundtrip. */
    [SYMBOL_FLOOR] = {
        .fn = xr_int_floor_method, .min_args = 0, .max_args = 0,
        .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
    },
    [SYMBOL_CEIL] = {
        .fn = xr_int_ceil_method, .min_args = 0, .max_args = 0,
        .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
    },
    [SYMBOL_ROUND] = {
        .fn = xr_int_round_method, .min_args = 0, .max_args = 0,
        .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
    },
    [SYMBOL_SQRT] = {
        .fn = xr_int_sqrt_method, .min_args = 0, .max_args = 0,
        .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
    },
    [SYMBOL_POW] = {
        .fn = xr_int_pow_method, .min_args = 0, .max_args = 1,
        .flags = XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC,
    },
};
