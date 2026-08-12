/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xrt_range_owner_freestanding.c - freestanding shared Range owner KAT
 */

#include "../test_framework.h"
#include "aot/xrt_core_freestanding.h"

TEST(freestanding_range_owner_matches_boundary_contract) {
    XrRangeCore half_open = xrt_range_semantics(INT64_MIN, INT64_MAX, false);
    ASSERT_EQ_INT(half_open.step, 1);
    ASSERT_FALSE(half_open.inclusive_end);
    ASSERT_EQ_INT(xr_range_core_length(half_open), INT64_MAX);
    ASSERT_TRUE(xr_range_core_contains(half_open, INT64_MAX - 1));
    ASSERT_FALSE(xr_range_core_contains(half_open, INT64_MAX));

    XrRangeCore inclusive = xrt_range_semantics(INT64_MAX - 1, INT64_MAX, true);
    ASSERT_EQ_INT(xr_range_core_length(inclusive), 2);
    ASSERT_TRUE(xr_range_core_contains(inclusive, INT64_MAX));
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Freestanding Range Owner");
RUN_TEST(freestanding_range_owner_matches_boundary_contract);
TEST_MAIN_END()
