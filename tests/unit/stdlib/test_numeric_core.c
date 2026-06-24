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
RUN_TEST(numeric_core_to_fixed_decimals_clamps);

TEST_MAIN_END()
