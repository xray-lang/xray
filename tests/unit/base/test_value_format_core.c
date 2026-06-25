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

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Value Format Core");
RUN_TEST(value_format_limits_are_stable);

TEST_MAIN_END()
