/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_map_set_abi.c - Unit tests for shared Map/Set ABI planning
 */

#include "../test_framework.h"
#include "runtime/value/xvalue.h"
#include "shared/xr_map_set_abi.h"

TEST(map_set_indices_size_uses_two_thirds_budget) {
    ASSERT_EQ_INT(xr_map_indices_size_for(0), 8);
    ASSERT_EQ_INT(xr_map_indices_size_for(1), 8);
    ASSERT_EQ_INT(xr_map_indices_size_for(5), 8);
    ASSERT_EQ_INT(xr_map_indices_size_for(6), 16);
    ASSERT_EQ_INT(xr_map_indices_size_for(10), 16);
    ASSERT_EQ_INT(xr_map_indices_size_for(11), 32);
}

TEST(map_set_entries_cap_keeps_requested_minimum) {
    ASSERT_EQ_INT(xr_map_set_entries_cap_for(8, 0), 5);
    ASSERT_EQ_INT(xr_map_set_entries_cap_for(8, 5), 5);
    ASSERT_EQ_INT(xr_map_set_entries_cap_for(8, 6), 6);
    ASSERT_EQ_INT(xr_map_set_entries_cap_for(16, 10), 10);
    ASSERT_EQ_INT(xr_map_set_entries_cap_for(16, 11), 11);
}

TEST(map_and_set_planners_match) {
    ASSERT_EQ_INT(xr_map_indices_size_for(1), xr_set_indices_size_for(1));
    ASSERT_EQ_INT(xr_map_indices_size_for(6), xr_set_indices_size_for(6));
    ASSERT_EQ_INT(xr_map_indices_size_for(11), xr_set_indices_size_for(11));
    ASSERT_EQ_INT(xr_map_indices_size_for(64), xr_set_indices_size_for(64));
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Map/Set ABI");
RUN_TEST(map_set_indices_size_uses_two_thirds_budget);
RUN_TEST(map_set_entries_cap_keeps_requested_minimum);
RUN_TEST(map_and_set_planners_match);

TEST_MAIN_END()
