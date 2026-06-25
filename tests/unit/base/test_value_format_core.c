/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_value_format_core.c - Unit tests for shared value formatting limits
 */

#include "../test_framework.h"
#include "shared/xr_value_format_core.h"

TEST(value_format_limits_are_stable) {
    ASSERT_EQ_INT(XR_VALUE_FORMAT_MAX_DEPTH, 3);
    ASSERT_EQ_INT(XR_VALUE_FORMAT_MAX_ELEMENTS, 32);
}

TEST(value_format_core_clamps_counts_and_depth) {
    ASSERT_EQ_INT(xr_value_format_depth_exceeded(3), 0);
    ASSERT_EQ_INT(xr_value_format_depth_exceeded(4), 1);
    ASSERT_EQ_INT((int) xr_value_format_limit_count(-1), 0);
    ASSERT_EQ_INT((int) xr_value_format_limit_count(8), 8);
    ASSERT_EQ_INT((int) xr_value_format_limit_count(40), 32);
    ASSERT_EQ_INT((int) xr_value_format_remaining_count(40, 32), 8);
    ASSERT_EQ_INT((int) xr_value_format_remaining_count(12, 32), 0);
}

TEST(value_format_core_writes_more_suffix) {
    char buf[32];
    int n = xr_value_format_more_suffix(buf, sizeof(buf), 40, 32);
    ASSERT_STR_EQ(buf, ", ...(8 more)");
    ASSERT_EQ_INT(n, 13);

    n = xr_value_format_more_suffix(buf, sizeof(buf), 12, 12);
    ASSERT_EQ_INT(n, 0);
    ASSERT_STR_EQ(buf, "");
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Value Format Core");
RUN_TEST(value_format_limits_are_stable);
RUN_TEST(value_format_core_clamps_counts_and_depth);
RUN_TEST(value_format_core_writes_more_suffix);

TEST_MAIN_END()
