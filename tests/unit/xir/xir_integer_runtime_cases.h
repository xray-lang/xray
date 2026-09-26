/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_integer_runtime_cases.h - Fixed-width integer semantic expectations
 *
 * KEY CONCEPT:
 *   Exhaustive byte inputs use bounded mathematical arithmetic as an oracle.
 */
#ifndef XIR_INTEGER_RUNTIME_CASES_H
#define XIR_INTEGER_RUNTIME_CASES_H
#include "xir/xxir_scalar.h"
#include <limits.h>

static const XrXirIntegerFormat integer_formats[] = {
    {8, true}, {16, true}, {32, true}, {64, true},
    {8, false}, {16, false}, {32, false}, {64, false}
};
static int byte_wrap(int value, bool sign) {
    int reduced = value % 256;
    if (reduced < 0) reduced += 256;
    return sign && reduced >= 128 ? reduced - 256 : reduced;
}
static void integer_expect(XrXirIntegerFormat format, XrXirArithmetic operation,
    int64_t left, int64_t right, int64_t expected) {
    int64_t actual = 19;
    CHECK(xr_xir_integer_arithmetic(format, operation, left, right, &actual) == XR_XIR_RUN_OK);
    CHECK(actual == expected);
}
static void integer_byte_pairs(bool sign) {
    XrXirIntegerFormat format = {8, sign};
    for (int a = 0; a < 256; ++a) for (int b = 0; b < 256; ++b) {
        int left = byte_wrap(a, sign), right = byte_wrap(b, sign), comparison = 17;
        integer_expect(format, XR_XIR_ARITH_ADD, left, right, byte_wrap(left + right, sign));
        integer_expect(format, XR_XIR_ARITH_SUB, left, right, byte_wrap(left - right, sign));
        integer_expect(format, XR_XIR_ARITH_MUL, left, right, byte_wrap(left * right, sign));
        integer_expect(format, XR_XIR_ARITH_AND, left, right, byte_wrap(a & b, sign));
        integer_expect(format, XR_XIR_ARITH_OR, left, right, byte_wrap(a | b, sign));
        integer_expect(format, XR_XIR_ARITH_XOR, left, right, byte_wrap(a ^ b, sign));
        CHECK(xr_xir_integer_compare(format, left, right, &comparison) == XR_XIR_RUN_OK);
        CHECK(comparison == (left > right) - (left < right));
        if (right) {
            integer_expect(format, XR_XIR_ARITH_DIV, left, right, byte_wrap(left / right, sign));
            integer_expect(format, XR_XIR_ARITH_REM, left, right, byte_wrap(left % right, sign));
        } else {
            int64_t value = 19;
            CHECK(xr_xir_integer_arithmetic(format, XR_XIR_ARITH_DIV, left, 0, &value) == XR_XIR_RUN_DIVIDE_BY_ZERO);
            CHECK(value == 0);
            value = 19;
            CHECK(xr_xir_integer_arithmetic(format, XR_XIR_ARITH_REM, left, 0, &value) == XR_XIR_RUN_DIVIDE_BY_ZERO);
            CHECK(value == 0);
        }
    }
}
static void integer_byte_shifts(bool sign) {
    XrXirIntegerFormat format = {8, sign};
    for (int a = 0; a < 256; ++a) for (int count = -129; count <= 129; ++count) {
        int value = byte_wrap(a, sign), left = value, right = value;
        int effective = (count % 64 + 64) % 64;
        for (int i = 0; i < effective; ++i) {
            left = byte_wrap(left * 2, sign);
            right = right / 2 - (right < 0 && right % 2 != 0);
        }
        integer_expect(format, XR_XIR_ARITH_SHL, value, count, left);
        integer_expect(format, XR_XIR_ARITH_SHR, value, count, right);
    }
}
static void integer_width_boundaries(void) {
    const int64_t minima[] = {INT8_MIN, INT16_MIN, INT32_MIN, INT64_MIN, 0, 0, 0, 0};
    const int64_t maxima[] = {INT8_MAX, INT16_MAX, INT32_MAX, INT64_MAX, UINT8_MAX, UINT16_MAX, UINT32_MAX, -1};
    for (unsigned f = 0; f < 8; ++f) {
        XrXirIntegerFormat format = integer_formats[f];
        int64_t minimum = minima[f], maximum = maxima[f];
        integer_expect(format, XR_XIR_ARITH_ADD, maximum, 1, minimum);
        integer_expect(format, XR_XIR_ARITH_SUB, minimum, 1, maximum);
        integer_expect(format, XR_XIR_ARITH_MUL, maximum, 2, format.is_signed ? -2 : (f == 7 ? -2 : maximum - 1));
        integer_expect(format, XR_XIR_ARITH_SHL, maximum, 64, maximum);
        integer_expect(format, XR_XIR_ARITH_SHR, maximum, -64, maximum);
        integer_expect(format, XR_XIR_ARITH_SHR, maximum, -1, f == 7 ? 1 : 0);
        int result = 17;
        CHECK(xr_xir_integer_compare(format, maximum, minimum, &result) == XR_XIR_RUN_OK && result == 1);
        CHECK(xr_xir_integer_compare(format, minimum, maximum, &result) == XR_XIR_RUN_OK && result == -1);
        CHECK(xr_xir_integer_compare(format, maximum, maximum, &result) == XR_XIR_RUN_OK && result == 0);
        if (format.is_signed) {
            integer_expect(format, XR_XIR_ARITH_DIV, minimum, -1, minimum);
            integer_expect(format, XR_XIR_ARITH_REM, minimum, -1, 0);
            integer_expect(format, XR_XIR_ARITH_SHR, minimum, format.bits - 1, -1);
            integer_expect(format, XR_XIR_ARITH_SHR, minimum, 64, minimum);
            integer_expect(format, XR_XIR_ARITH_DIV, -17, 5, -3);
            integer_expect(format, XR_XIR_ARITH_REM, -17, 5, -2);
            integer_expect(format, XR_XIR_ARITH_DIV, 17, -5, -3);
            integer_expect(format, XR_XIR_ARITH_REM, 17, -5, 2);
        } else {
            integer_expect(format, XR_XIR_ARITH_DIV, maximum, 2, maxima[f - 4]);
            integer_expect(format, XR_XIR_ARITH_REM, maximum, 2, 1);
        }
    }
    integer_expect(integer_formats[7], XR_XIR_ARITH_DIV, -1, INT64_MIN, 1);
    integer_expect(integer_formats[7], XR_XIR_ARITH_REM, -1, INT64_MIN, INT64_MAX);
    integer_expect(integer_formats[7], XR_XIR_ARITH_DIV, INT64_MIN, 2, INT64_C(4611686018427387904));
    integer_expect(integer_formats[7], XR_XIR_ARITH_MUL, INT64_MIN, 2, 0);
}
static void integer_conversion_cases(void) {
    static const struct { unsigned from, to; int64_t value, expected; } cases[] = {
        {0, 7, -1, -1}, {0, 5, -128, 65408}, {4, 1, 255, 255}, {4, 0, 255, -1},
        {1, 0, 128, -128}, {1, 4, -129, 127}, {2, 1, 65535, -1}, {2, 5, -32769, 32767},
        {3, 2, INT64_MIN, 0}, {3, 6, INT64_MAX, UINT32_MAX}, {3, 7, INT64_MIN, INT64_MIN},
        {6, 3, UINT32_MAX, INT64_C(4294967295)}, {6, 2, UINT32_MAX, -1},
        {7, 3, -1, -1}, {7, 2, INT64_MIN, 0}, {7, 0, -128, -128}, {7, 4, -128, 128},
        {5, 1, 32768, -32768}, {1, 3, -32768, -32768}, {0, 1, -128, -128}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int64_t value = 19;
        CHECK(xr_xir_integer_convert(integer_formats[cases[i].from], integer_formats[cases[i].to], cases[i].value, &value) == XR_XIR_RUN_OK);
        CHECK(value == cases[i].expected);
    }
    for (unsigned sign = 0; sign < 2; ++sign) for (int a = 0; a < 256; ++a) {
        XrXirIntegerFormat source = {8, sign != 0};
        for (unsigned f = 0; f < 8; ++f) {
            int64_t middle = 19, result = 19, original = byte_wrap(a, sign != 0);
            CHECK(xr_xir_integer_convert(source, integer_formats[f], original, &middle) == XR_XIR_RUN_OK);
            CHECK(xr_xir_integer_convert(integer_formats[f], source, middle, &result) == XR_XIR_RUN_OK);
            CHECK(result == original);
        }
    }
}
static void integer_rejections(void) {
    const uint32_t widths[] = {0, 1, 7, 9, 15, 31, 63, 65, 128, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        XrXirIntegerFormat invalid = {widths[i], true};
        int64_t value = 19; int comparison = 17;
        CHECK(xr_xir_integer_arithmetic(invalid, XR_XIR_ARITH_ADD, 0, 0, &value) == XR_XIR_RUN_BAD_ARGUMENT && value == 0);
        CHECK(xr_xir_integer_compare(invalid, 0, 0, &comparison) == XR_XIR_RUN_BAD_ARGUMENT && comparison == 0);
        CHECK(xr_xir_integer_convert(invalid, integer_formats[3], 0, &value) == XR_XIR_RUN_BAD_ARGUMENT && value == 0);
        CHECK(xr_xir_integer_convert(integer_formats[3], invalid, 0, &value) == XR_XIR_RUN_BAD_ARGUMENT && value == 0);
    }
    const int64_t invalid_payloads[] = {128, 32768, INT64_C(2147483648), -129, -1, -1, -1};
    const unsigned formats[] = {0, 1, 2, 0, 4, 5, 6};
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        XrXirIntegerFormat format = integer_formats[formats[i]];
        int64_t value = 19, input = invalid_payloads[i]; int comparison = 17;
        CHECK(xr_xir_integer_arithmetic(format, XR_XIR_ARITH_ADD, input, 0, &value) == XR_XIR_RUN_BAD_ARGUMENT && value == 0);
        CHECK(xr_xir_integer_arithmetic(format, XR_XIR_ARITH_ADD, 0, input, &value) == XR_XIR_RUN_BAD_ARGUMENT && value == 0);
        CHECK(xr_xir_integer_compare(format, 0, input, &comparison) == XR_XIR_RUN_BAD_ARGUMENT && comparison == 0);
        CHECK(xr_xir_integer_compare(format, input, 0, &comparison) == XR_XIR_RUN_BAD_ARGUMENT && comparison == 0);
        CHECK(xr_xir_integer_convert(format, integer_formats[3], input, &value) == XR_XIR_RUN_BAD_ARGUMENT && value == 0);
    }
    int64_t value = 19;
    CHECK(xr_xir_integer_arithmetic(integer_formats[3], (XrXirArithmetic) 99, 0, 0, &value) == XR_XIR_RUN_BAD_ARGUMENT && value == 0);
    CHECK(xr_xir_integer_arithmetic(integer_formats[3], XR_XIR_ARITH_ADD, 0, 0, NULL) == XR_XIR_RUN_BAD_ARGUMENT);
    CHECK(xr_xir_integer_compare(integer_formats[3], 0, 0, NULL) == XR_XIR_RUN_BAD_ARGUMENT);
    CHECK(xr_xir_integer_convert(integer_formats[3], integer_formats[3], 0, NULL) == XR_XIR_RUN_BAD_ARGUMENT);
}
static void integer_runtime_cases(void) {
    integer_byte_pairs(false); integer_byte_pairs(true);
    integer_byte_shifts(false); integer_byte_shifts(true);
    integer_width_boundaries(); integer_conversion_cases(); integer_rejections();
    puts("Integer runtime: 131072 byte pairs, 132608 shifts, eight widths/signs and conversion boundaries passed");
}
#endif // XIR_INTEGER_RUNTIME_CASES_H
