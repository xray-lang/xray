/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_numeric_core.c - Unit tests for runtime-neutral numeric method helpers
 */

#include "../test_framework.h"
#include "shared/xr_numeric_core.h"
#include <stdint.h>
#include <string.h>

static void assert_format_i64(int64_t value, const char *expected) {
    char buf[32];
    int len = xr_numeric_core_format_i64(buf, sizeof(buf), value);
    ASSERT_EQ_INT(len, (int64_t) strlen(expected));
    ASSERT(strcmp(buf, expected) == 0);
}

static void assert_format_hex(int64_t value, const char *expected) {
    char buf[32];
    int len = xr_numeric_core_format_i64_hex(buf, sizeof(buf), value);
    ASSERT_EQ_INT(len, (int64_t) strlen(expected));
    ASSERT(strcmp(buf, expected) == 0);
}

TEST(numeric_core_format_i64_handles_boundaries) {
    assert_format_i64(0, "0");
    assert_format_i64(42, "42");
    assert_format_i64(-42, "-42");
    assert_format_i64(INT64_MAX, "9223372036854775807");
    assert_format_i64(INT64_MIN, "-9223372036854775808");
}

TEST(numeric_core_format_i64_rejects_short_buffer) {
    char buf[4] = {0};
    ASSERT_EQ_INT(xr_numeric_core_format_i64(buf, sizeof(buf), 12345), -1);
}

TEST(numeric_core_hex_matches_signed_magnitude_rule) {
    assert_format_hex(0, "0x0");
    assert_format_hex(15, "0xF");
    assert_format_hex(255, "0xFF");
    assert_format_hex(-15, "-0xF");
    assert_format_hex(INT64_MIN, "-0x8000000000000000");
}

TEST(numeric_core_abs_wraps_int64_min) {
    ASSERT_EQ_INT(xr_numeric_core_i64_abs_wrap(0), 0);
    ASSERT_EQ_INT(xr_numeric_core_i64_abs_wrap(-42), 42);
    ASSERT_EQ_INT(xr_numeric_core_i64_abs_wrap(INT64_MIN), INT64_MIN);
}

TEST(numeric_core_math_abs_preserves_int_or_promotes_min) {
    XrNumericCoreI64AbsResult zero = xr_numeric_core_i64_math_abs(0);
    ASSERT_FALSE(zero.is_float);
    ASSERT_EQ_INT(zero.int_value, 0);

    XrNumericCoreI64AbsResult neg = xr_numeric_core_i64_math_abs(-42);
    ASSERT_FALSE(neg.is_float);
    ASSERT_EQ_INT(neg.int_value, 42);

    XrNumericCoreI64AbsResult min = xr_numeric_core_i64_math_abs(INT64_MIN);
    ASSERT_TRUE(min.is_float);
    ASSERT(min.float_value == (double) INT64_MAX + 1.0);
}

TEST(numeric_core_integer_arithmetic_wraps) {
    ASSERT_EQ_INT(xr_numeric_core_i64_add_wrap(INT64_MAX, 1), INT64_MIN);
    ASSERT_EQ_INT(xr_numeric_core_i64_sub_wrap(INT64_MIN, 1), INT64_MAX);
    ASSERT_EQ_INT(xr_numeric_core_i64_mul_wrap(INT64_MAX, 2), -2);
    ASSERT_EQ_INT(xr_numeric_core_i64_neg_wrap(INT64_MIN), INT64_MIN);
}

TEST(numeric_core_integer_div_mod_edges_match_language) {
    ASSERT_EQ_INT(xr_numeric_core_i64_div_wrap(INT64_MIN, -1), INT64_MIN);
    ASSERT_EQ_INT(xr_numeric_core_i64_mod_wrap(INT64_MIN, -1), 0);
    ASSERT_EQ_INT(xr_numeric_core_i64_div_wrap(7, -3), -2);
    ASSERT_EQ_INT(xr_numeric_core_i64_mod_wrap(7, -3), 1);
}

TEST(numeric_core_shift_counts_are_mod64) {
    ASSERT_EQ_INT(xr_numeric_core_i64_shl_wrap(12345, 64), 12345);
    ASSERT_EQ_INT(xr_numeric_core_i64_shl_wrap(12345, 65), 24690);
    ASSERT_EQ_INT(xr_numeric_core_i64_shr_wrap(12345, 70), 192);
    ASSERT_EQ_INT(xr_numeric_core_i64_shl_wrap(1, -1), INT64_MIN);
    ASSERT_EQ_INT(xr_numeric_core_i64_shr_wrap(-8, 1), -4);
    ASSERT_EQ_INT(xr_numeric_core_i64_shr_wrap(INT64_MIN, 63), -1);
}

TEST(numeric_core_to_fixed_decimals_clamps) {
    ASSERT_EQ_INT(xr_numeric_core_to_fixed_decimals(-10), 0);
    ASSERT_EQ_INT(xr_numeric_core_to_fixed_decimals(3), 3);
    ASSERT_EQ_INT(xr_numeric_core_to_fixed_decimals(99), XR_TOFIXED_MAX_DECIMALS);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Numeric Core - Scalar Methods");
RUN_TEST(numeric_core_format_i64_handles_boundaries);
RUN_TEST(numeric_core_format_i64_rejects_short_buffer);
RUN_TEST(numeric_core_hex_matches_signed_magnitude_rule);
RUN_TEST(numeric_core_abs_wraps_int64_min);
RUN_TEST(numeric_core_math_abs_preserves_int_or_promotes_min);
RUN_TEST(numeric_core_integer_arithmetic_wraps);
RUN_TEST(numeric_core_integer_div_mod_edges_match_language);
RUN_TEST(numeric_core_shift_counts_are_mod64);
RUN_TEST(numeric_core_to_fixed_decimals_clamps);

TEST_MAIN_END()
