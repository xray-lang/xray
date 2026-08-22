/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_string_parse_core.h - Strict decimal string->number parsing shared by the
 * VM, hosted AOT and freestanding AOT.
 *
 * Two properties make this a separate header instead of a section of
 * xr_string_core.h:
 *
 *   1. It is libc-free. The freestanding profile links with -nostdlib and may
 *      only import xr_hook_panic / xr_hook_write / mem* intrinsics, so strtoll,
 *      strtod and malloc are all unavailable there. Every profile therefore
 *      runs this same hand-rolled decimal parser, which is what makes the
 *      accept/reject decision identical on all three rather than "whatever the
 *      host libc happened to do".
 *
 *   2. It is strict. The accepted grammar is the whole string:
 *
 *        int:   ws* [+-]? digit+ ws*
 *        float: ws* [+-]? ( digit* ('.' digit*)? ) ( [eE] [+-]? digit+ )? ws*
 *               with at least one integer-or-fraction digit
 *
 *      Any trailing residue rejects the input, so "12abc" fails instead of
 *      yielding 12. This is the exact scalar parse grammar shared by every
 *      backend; it
 *      also rejects the hex-float and inf/nan spellings a libc strtod would
 *      otherwise accept behind the language's back.
 *
 * Integer results are exact and overflow is rejected rather than saturated.
 * Float results take the Clinger fast path (exact mantissa scaled by an exact
 * power of ten, one rounding) whenever the mantissa fits in 2^53 and the
 * decimal exponent is within the exactly-representable table, which is
 * correctly rounded and therefore matches what strtod produced before. Inputs
 * outside that window fall back to iterated scaling, which stays within a few
 * ulp and, being the same code everywhere, stays identical across profiles.
 * A grammatically valid magnitude that overflows binary64 yields an infinity
 * rather than a parse failure, matching the previous strtod behaviour.
 */

#ifndef XRAY_SHARED_XR_STRING_PARSE_CORE_H
#define XRAY_SHARED_XR_STRING_PARSE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrStringParseIntResult {
    bool ok;
    int64_t value;
} XrStringParseIntResult;

typedef struct XrStringParseFloatResult {
    bool ok;
    double value;
} XrStringParseFloatResult;

/* Powers of ten that are exactly representable in binary64: 10^22 is the last
 * one whose significand still fits in 53 bits. */
#define XR_STRING_PARSE_EXACT_POW10_MAX 22

/* Kept function-local so including this header does not plant an unused
 * read-only table in every translation unit. */
static inline double xr_string_parse_pow10(int exponent) {
    static const double table[XR_STRING_PARSE_EXACT_POW10_MAX + 1] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    return table[exponent];
}

/* Largest mantissa an exact double can hold without losing low bits. */
#define XR_STRING_PARSE_MANTISSA_EXACT_MAX UINT64_C(9007199254740992) /* 2^53 */

/* Significant digits kept before further digits only move the exponent. 19 is
 * the most that always fits in uint64_t. */
#define XR_STRING_PARSE_MAX_SIGNIFICANT_DIGITS 19

static inline bool xr_string_parse_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static inline bool xr_string_parse_is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

static inline double xr_string_parse_infinity(void) {
    /* Built from bits so no libm/HUGE_VAL dependency and no overflow warning. */
    union {
        uint64_t bits;
        double value;
    } u;
    u.bits = UINT64_C(0x7FF0000000000000);
    return u.value;
}

/* Scale mantissa by 10^exp10 for inputs outside the exact fast path. Same code
 * on every profile, so every profile lands on the same bits. */
static inline double xr_string_parse_scale_pow10(double mantissa, int exp10) {
    double value = mantissa;
    int remaining = exp10;
    if (remaining > 0) {
        while (remaining > XR_STRING_PARSE_EXACT_POW10_MAX) {
            value *= xr_string_parse_pow10(XR_STRING_PARSE_EXACT_POW10_MAX);
            remaining -= XR_STRING_PARSE_EXACT_POW10_MAX;
            if (value > 1.7976931348623157e308)
                return xr_string_parse_infinity();
        }
        value *= xr_string_parse_pow10(remaining);
    } else if (remaining < 0) {
        remaining = -remaining;
        while (remaining > XR_STRING_PARSE_EXACT_POW10_MAX) {
            value /= xr_string_parse_pow10(XR_STRING_PARSE_EXACT_POW10_MAX);
            remaining -= XR_STRING_PARSE_EXACT_POW10_MAX;
            if (value == 0.0)
                return 0.0;
        }
        value /= xr_string_parse_pow10(remaining);
    }
    return value;
}

/*
 * Strict decimal integer parse over the whole [data, data+len) span.
 *
 * The accumulator runs negative so INT64_MIN stays representable, and both
 * overflow guards fire before the operation that would overflow -- the same
 * shape used by i64.parse/tryParse, so every backend rejects exactly the same
 * magnitudes.
 */
static inline XrStringParseIntResult xr_string_parse_int64(const char *data, size_t len) {
    XrStringParseIntResult out = {false, 0};
    if (!data)
        return out;

    size_t pos = 0;
    while (pos < len && xr_string_parse_is_space((unsigned char) data[pos]))
        pos++;

    bool negative = false;
    if (pos < len && (data[pos] == '+' || data[pos] == '-')) {
        negative = data[pos] == '-';
        pos++;
    }

    size_t digit_start = pos;
    int64_t limit = negative ? INT64_MIN : -INT64_MAX;
    int64_t multiply_limit = limit / 10;
    int64_t accumulated = 0;
    while (pos < len && xr_string_parse_is_digit((unsigned char) data[pos])) {
        int64_t digit = (int64_t) (data[pos] - '0');
        if (accumulated < multiply_limit)
            return out;
        accumulated *= 10;
        if (accumulated < limit + digit)
            return out;
        accumulated -= digit;
        pos++;
    }
    if (pos == digit_start)
        return out;

    while (pos < len && xr_string_parse_is_space((unsigned char) data[pos]))
        pos++;
    if (pos != len)
        return out;

    out.ok = true;
    out.value = negative ? accumulated : -accumulated;
    return out;
}

/*
 * Strict decimal float parse over the whole [data, data+len) span.
 *
 * Grammar and value production are deliberately separate: the scan below owns
 * accept/reject (and is bit-for-bit the same decision on every profile), while
 * the mantissa/exponent pair it produces is turned into a double by the shared
 * scaling above.
 */
static inline XrStringParseFloatResult xr_string_parse_float64(const char *data, size_t len) {
    XrStringParseFloatResult out = {false, 0.0};
    if (!data)
        return out;

    size_t pos = 0;
    while (pos < len && xr_string_parse_is_space((unsigned char) data[pos]))
        pos++;

    bool negative = false;
    if (pos < len && (data[pos] == '+' || data[pos] == '-')) {
        negative = data[pos] == '-';
        pos++;
    }

    uint64_t mantissa = 0;
    int significant_digits = 0;
    int exponent_adjust = 0;
    bool saw_nonzero = false;
    size_t integer_digits = 0;
    while (pos < len && xr_string_parse_is_digit((unsigned char) data[pos])) {
        unsigned digit = (unsigned) (data[pos] - '0');
        if (digit != 0)
            saw_nonzero = true;
        if (significant_digits < XR_STRING_PARSE_MAX_SIGNIFICANT_DIGITS && saw_nonzero) {
            mantissa = mantissa * 10u + digit;
            significant_digits++;
        } else if (saw_nonzero) {
            /* Past the retained window: the digit only shifts the exponent. */
            exponent_adjust++;
        }
        integer_digits++;
        pos++;
    }

    size_t fraction_digits = 0;
    if (pos < len && data[pos] == '.') {
        pos++;
        while (pos < len && xr_string_parse_is_digit((unsigned char) data[pos])) {
            unsigned digit = (unsigned) (data[pos] - '0');
            if (digit != 0)
                saw_nonzero = true;
            if (significant_digits < XR_STRING_PARSE_MAX_SIGNIFICANT_DIGITS && saw_nonzero) {
                mantissa = mantissa * 10u + digit;
                significant_digits++;
                exponent_adjust--;
            } else if (!saw_nonzero) {
                /* Leading zeros of a pure fraction: not significant, but they
                 * still scale whatever digits follow. */
                exponent_adjust--;
            }
            fraction_digits++;
            pos++;
        }
    }
    if (integer_digits + fraction_digits == 0)
        return out;

    int exponent = 0;
    if (pos < len && (data[pos] == 'e' || data[pos] == 'E')) {
        pos++;
        bool exponent_negative = false;
        if (pos < len && (data[pos] == '+' || data[pos] == '-')) {
            exponent_negative = data[pos] == '-';
            pos++;
        }
        size_t exponent_digits = 0;
        while (pos < len && xr_string_parse_is_digit((unsigned char) data[pos])) {
            if (exponent < 100000)
                exponent = exponent * 10 + (data[pos] - '0');
            exponent_digits++;
            pos++;
        }
        if (exponent_digits == 0)
            return out;
        if (exponent_negative)
            exponent = -exponent;
    }

    while (pos < len && xr_string_parse_is_space((unsigned char) data[pos]))
        pos++;
    if (pos != len)
        return out;

    double value;
    if (!saw_nonzero) {
        value = 0.0;
    } else {
        int exp10 = exponent + exponent_adjust;
        double mantissa_double = (double) mantissa;
        if (mantissa <= XR_STRING_PARSE_MANTISSA_EXACT_MAX &&
            exp10 >= -XR_STRING_PARSE_EXACT_POW10_MAX && exp10 <= XR_STRING_PARSE_EXACT_POW10_MAX) {
            /* Both operands exact, single rounding: correctly rounded result. */
            value = exp10 >= 0 ? mantissa_double * xr_string_parse_pow10(exp10)
                               : mantissa_double / xr_string_parse_pow10(-exp10);
        } else {
            value = xr_string_parse_scale_pow10(mantissa_double, exp10);
        }
    }

    out.ok = true;
    out.value = negative ? -value : value;
    return out;
}

#endif  // XRAY_SHARED_XR_STRING_PARSE_CORE_H
