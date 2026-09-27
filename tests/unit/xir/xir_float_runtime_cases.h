/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_float_runtime_cases.h - Exact bit expectations independent of host rounding
 *
 * KEY CONCEPT:
 *   Rational conversion witnesses and IEEE ordering precede pipeline admission.
 */
#ifndef XIR_FLOAT_RUNTIME_CASES_H
#define XIR_FLOAT_RUNTIME_CASES_H
#include "xir/xxir_float.h"
#include <fenv.h>
#include "xir_float_arithmetic_cases.h"
#include "xir_float_format_cases.h"
static int64_t float_test_payload(uint64_t bits) {
    return bits <= INT64_MAX ? (int64_t) bits : -1 - (int64_t) (UINT64_MAX - bits);
}
static void float_runtime_vectors(void) {
    static const struct { uint32_t source, target; uint64_t input, output; } floating[] = {
#define XIR_F2F(source, target, input, output) {source, target, input, output},
#define XIR_I2F(width, sign, target, input, output)
#define XIR_F2I(source, width, sign, input, status, output)
#include "xir_float_vectors.def"
#undef XIR_F2F
#undef XIR_I2F
#undef XIR_F2I
    };
    static const struct { XrXirIntegerFormat source; uint32_t target; uint64_t input, output; } integers[] = {
#define XIR_F2F(source, target, input, output)
#define XIR_I2F(width, sign, target, input, output) {{width, sign}, target, input, output},
#define XIR_F2I(source, width, sign, input, status, output)
#include "xir_float_vectors.def"
#undef XIR_F2F
#undef XIR_I2F
#undef XIR_F2I
    };
    static const struct { uint32_t source; XrXirIntegerFormat target; uint64_t input; XrXirNumericStatus status; uint64_t output; } truncations[] = {
#define XIR_F2F(source, target, input, output)
#define XIR_I2F(width, sign, target, input, output)
#define XIR_F2I(source, width, sign, input, status, output) {source, {width, sign}, input, status, output},
#include "xir_float_vectors.def"
#undef XIR_F2F
#undef XIR_I2F
#undef XIR_F2I
    };
    _Static_assert(sizeof(floating) / sizeof(floating[0]) == 252, "floating witnesses");
    _Static_assert(sizeof(integers) / sizeof(integers[0]) == 284, "integer witnesses");
    _Static_assert(sizeof(truncations) / sizeof(truncations[0]) == 248, "truncation witnesses");
    for (size_t i = 0; i < sizeof(floating) / sizeof(floating[0]); ++i) {
        uint64_t output = UINT64_MAX;
        CHECK(xr_xir_float_convert(floating[i].source, floating[i].target, floating[i].input, &output) == XR_XIR_NUMERIC_OK);
        CHECK(output == floating[i].output);
    }
    for (size_t i = 0; i < sizeof(integers) / sizeof(integers[0]); ++i) {
        uint64_t output = UINT64_MAX;
        CHECK(xr_xir_integer_to_float(integers[i].source, integers[i].target, float_test_payload(integers[i].input), &output) == XR_XIR_NUMERIC_OK);
        CHECK(output == integers[i].output);
    }
    for (size_t i = 0; i < sizeof(truncations) / sizeof(truncations[0]); ++i) {
        int64_t output = 123;
        CHECK(xr_xir_float_to_integer(truncations[i].source, truncations[i].target, truncations[i].input, &output) == truncations[i].status);
        CHECK((uint64_t) output == truncations[i].output);
    }
}
static void float_runtime_ordering(void) {
    static const struct { uint64_t narrow, wide; int rank; } values[] = {
        {UINT64_C(0xff800000), UINT64_C(0xfff0000000000000), 0},
        {UINT64_C(0xff7fffff), UINT64_C(0xffefffffffffffff), 1},
        {UINT64_C(0xbf800000), UINT64_C(0xbff0000000000000), 2},
        {UINT64_C(0x80000001), UINT64_C(0x8000000000000001), 3},
        {UINT64_C(0x80000000), UINT64_C(0x8000000000000000), 4},
        {0, 0, 4}, {1, 1, 5},
        {UINT64_C(0x3f800000), UINT64_C(0x3ff0000000000000), 6},
        {UINT64_C(0x7f7fffff), UINT64_C(0x7fefffffffffffff), 7},
        {UINT64_C(0x7f800000), UINT64_C(0x7ff0000000000000), 8},
        {UINT64_C(0xff800001), UINT64_C(0xfff0000000000001), -1},
        {UINT64_C(0x7fc12345), UINT64_C(0x7ff8000000012345), -1}
    };
    for (uint32_t width = 32; width <= 64; width += 32) {
        uint64_t sign = UINT64_C(1) << (width - 1);
        uint64_t nan = width == 32 ? UINT64_C(0x7fc00000) : UINT64_C(0x7ff8000000000000);
        for (size_t a = 0; a < 12; ++a) {
            uint64_t left = width == 32 ? values[a].narrow : values[a].wide, negated = 0;
            CHECK(xr_xir_float_negate(width, left, &negated) == XR_XIR_NUMERIC_OK);
            CHECK(negated == (values[a].rank < 0 ? nan : left ^ sign));
            for (size_t b = 0; b < 12; ++b) {
                uint64_t right = width == 32 ? values[b].narrow : values[b].wide;
                int expected = values[a].rank < 0 || values[b].rank < 0 ? XR_XIR_FLOAT_UNORDERED :
                    (values[a].rank > values[b].rank) - (values[a].rank < values[b].rank);
                XrXirFloatOrder order;
                CHECK(xr_xir_float_compare(width, left, right, &order) == XR_XIR_NUMERIC_OK && (int) order == expected);
            }
        }
    }
}
static void float_runtime_roundtrips(void) {
    uint32_t sample = UINT32_C(0x1871fac3);
    for (uint32_t i = 0; i < 100000; ++i) {
        sample ^= sample << 13; sample ^= sample >> 17; sample ^= sample << 5;
        uint64_t wide = 0, narrow = 0;
        CHECK(xr_xir_float_convert(32, 64, sample, &wide) == XR_XIR_NUMERIC_OK);
        CHECK(xr_xir_float_convert(64, 32, wide, &narrow) == XR_XIR_NUMERIC_OK);
        bool nan = (sample & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) && (sample & UINT32_C(0x007fffff));
        CHECK(narrow == (nan ? UINT32_C(0x7fc00000) : sample));
    }
    for (unsigned sign = 0; sign < 2; ++sign) for (int64_t i = 0; i < 256; ++i) {
        int64_t input = sign ? i - 128 : i;
        XrXirIntegerFormat format = {8, sign != 0};
        for (uint32_t width = 32; width <= 64; width += 32) {
            uint64_t bits = 0; int64_t output = 1000;
            CHECK(xr_xir_integer_to_float(format, width, input, &bits) == XR_XIR_NUMERIC_OK);
            CHECK(xr_xir_float_to_integer(width, format, bits, &output) == XR_XIR_NUMERIC_OK && output == input);
        }
    }
}
static void float_runtime_rejections(void) {
    static const uint32_t invalid[] = {0, 1, 8, 16, 31, 33, 63, 65, UINT32_MAX};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        uint64_t output = 123; int64_t integer = 123; XrXirFloatOrder order = XR_XIR_FLOAT_GREATER;
        CHECK(xr_xir_float_convert(invalid[i], 64, 0, &output) == XR_XIR_NUMERIC_BAD_ARGUMENT && !output);
        output = 123;
        CHECK(xr_xir_float_convert(64, invalid[i], 0, &output) == XR_XIR_NUMERIC_BAD_ARGUMENT && !output);
        output = 123;
        CHECK(xr_xir_integer_to_float((XrXirIntegerFormat) {64, true}, invalid[i], 0, &output) == XR_XIR_NUMERIC_BAD_ARGUMENT && !output);
        CHECK(xr_xir_float_to_integer(invalid[i], (XrXirIntegerFormat) {64, true}, 0, &integer) == XR_XIR_NUMERIC_BAD_ARGUMENT && !integer);
        CHECK(xr_xir_float_compare(invalid[i], 0, 0, &order) == XR_XIR_NUMERIC_BAD_ARGUMENT && order == XR_XIR_FLOAT_EQUAL);
        output = 123;
        CHECK(xr_xir_float_negate(invalid[i], 0, &output) == XR_XIR_NUMERIC_BAD_ARGUMENT && !output);
    }
    uint64_t output = 123; int64_t integer = 123; XrXirFloatOrder order = XR_XIR_FLOAT_GREATER;
    CHECK(xr_xir_float_convert(32, 64, UINT64_C(0x100000000), &output) == XR_XIR_NUMERIC_BAD_ARGUMENT && !output);
    CHECK(xr_xir_float_to_integer(32, (XrXirIntegerFormat) {64, true}, UINT64_C(0x100000000), &integer) == XR_XIR_NUMERIC_BAD_ARGUMENT && !integer);
    CHECK(xr_xir_float_compare(32, 0, UINT64_C(0x100000000), &order) == XR_XIR_NUMERIC_BAD_ARGUMENT && order == XR_XIR_FLOAT_EQUAL);
    output = 123;
    CHECK(xr_xir_float_negate(32, UINT64_C(0x100000000), &output) == XR_XIR_NUMERIC_BAD_ARGUMENT && !output);
    output = 123;
    CHECK(xr_xir_integer_to_float((XrXirIntegerFormat) {8, true}, 64, 128, &output) == XR_XIR_NUMERIC_BAD_ARGUMENT && !output);
    output = 123;
    CHECK(xr_xir_integer_to_float((XrXirIntegerFormat) {8, false}, 64, -1, &output) == XR_XIR_NUMERIC_BAD_ARGUMENT && !output);
    output = 123;
    CHECK(xr_xir_integer_to_float((XrXirIntegerFormat) {0, false}, 64, 0, &output) == XR_XIR_NUMERIC_BAD_ARGUMENT && !output);
    integer = 123;
    CHECK(xr_xir_float_to_integer(64, (XrXirIntegerFormat) {0, true}, 0, &integer) == XR_XIR_NUMERIC_BAD_ARGUMENT && !integer);
    CHECK(xr_xir_float_convert(32, 64, 0, NULL) == XR_XIR_NUMERIC_BAD_ARGUMENT);
    CHECK(xr_xir_integer_to_float((XrXirIntegerFormat) {8, false}, 64, 0, NULL) == XR_XIR_NUMERIC_BAD_ARGUMENT);
    CHECK(xr_xir_float_to_integer(64, (XrXirIntegerFormat) {8, false}, 0, NULL) == XR_XIR_NUMERIC_BAD_ARGUMENT);
    CHECK(xr_xir_float_compare(64, 0, 0, NULL) == XR_XIR_NUMERIC_BAD_ARGUMENT);
    CHECK(xr_xir_float_negate(64, 0, NULL) == XR_XIR_NUMERIC_BAD_ARGUMENT);
}
static void float_runtime_cases(void) {
    fenv_t original;
    CHECK(fegetenv(&original) == 0);
    static const int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(fesetround(modes[i]) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0);
        CHECK(feraiseexcept(FE_INEXACT) == 0);
        int exceptions = fetestexcept(FE_ALL_EXCEPT);
        float_runtime_vectors(); float_runtime_ordering(); float_runtime_rejections();
        float_arithmetic_cases();
        float_format_cases();
        CHECK(fegetround() == modes[i] && fetestexcept(FE_ALL_EXCEPT) == exceptions);
    }
    CHECK(fesetenv(&original) == 0);
    float_runtime_roundtrips();
    puts("Float core: 784 rational vectors, 288 ordered pairs, 4 rounding modes, 100000 bit and 1024 byte roundtrips");
}
#endif
