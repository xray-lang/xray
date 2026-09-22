/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_integer_division_core.h - Exact-width integer division and remainder
 */

#ifndef XR_INTEGER_DIVISION_CORE_H
#define XR_INTEGER_DIVISION_CORE_H

#ifndef XR_INTEGER_CONVERSION_CORE_H
#include "xr_integer_conversion_core.h"
#endif

/* The caller proves a nonzero divisor after exact-width normalization. A
 * positive-divisor proof also permits removing the signed overflow probe. */
static inline uint64_t xr_integer_divmod_nonzero_bits(uint64_t left, uint64_t right,
                                                       uint8_t width, bool is_signed,
                                                       bool remainder, bool positive_divisor) {
    uint64_t a = xr_integer_convert_bits(left, width, is_signed, width, is_signed);
    uint64_t b = xr_integer_convert_bits(right, width, is_signed, width, is_signed);
    uint64_t result;
    if (is_signed) {
        int64_t signed_left = xr_integer_signed_from_bits(a);
        int64_t signed_right = xr_integer_signed_from_bits(b);
        if (!positive_divisor && signed_right == -1) {
            result = remainder ? UINT64_C(0) : UINT64_C(0) - a;
        } else {
            result = (uint64_t) (remainder ? signed_left % signed_right
                                           : signed_left / signed_right);
        }
    } else {
        result = remainder ? a % b : a / b;
    }
    return xr_integer_convert_bits(result, 64u, false, width, is_signed);
}

typedef struct XrIntegerDivModResult {
    uint64_t bits;
    bool divisor_is_zero;
} XrIntegerDivModResult;

/* Width and signedness come from an admitted exact integer type. The result
 * is a bit pattern; consumers publish division/modulo faults in their own
 * panic or trap channel without duplicating the numeric rule. */
static inline XrIntegerDivModResult xr_integer_divmod_eval(uint64_t left, uint64_t right,
                                                           uint8_t width, bool is_signed,
                                                           bool remainder) {
    XrIntegerDivModResult result = {UINT64_C(0), false};
    if (xr_integer_convert_bits(right, width, is_signed, width, is_signed) == 0u) {
        result.divisor_is_zero = true;
        return result;
    }
    result.bits = xr_integer_divmod_nonzero_bits(left, right, width, is_signed, remainder, false);
    return result;
}

#endif  // XR_INTEGER_DIVISION_CORE_H
