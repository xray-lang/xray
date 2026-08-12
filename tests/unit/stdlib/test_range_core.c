/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_range_core.c - Unit tests for runtime-neutral Range core helpers
 */

#include "../test_framework.h"
#include "shared/xr_range_core.h"

TEST(range_core_length_uses_half_open_forward_bounds) {
    ASSERT_EQ_INT(xr_range_core_length(xr_range_core_make(1, 10, 1)), 9);
    ASSERT_EQ_INT(xr_range_core_length(xr_range_core_make(1, 10, 2)), 5);
    ASSERT_EQ_INT(xr_range_core_length(xr_range_core_make(5, 5, 1)), 0);
    ASSERT_EQ_INT(xr_range_core_length(xr_range_core_make(10, 1, 1)), 0);
    ASSERT_EQ_INT(xr_range_core_length(xr_range_core_make(1, 10, 0)), 0);
}

TEST(range_core_length_uses_half_open_reverse_bounds) {
    ASSERT_EQ_INT(xr_range_core_length(xr_range_core_make(10, 1, -1)), 9);
    ASSERT_EQ_INT(xr_range_core_length(xr_range_core_make(10, 1, -2)), 5);
    ASSERT_EQ_INT(xr_range_core_length(xr_range_core_make(5, 5, -1)), 0);
    ASSERT_EQ_INT(xr_range_core_length(xr_range_core_make(1, 10, -1)), 0);
}

TEST(range_core_length_and_contains_use_inclusive_bounds) {
    XrRangeCore forward = xr_range_core_make_with_bound(1, 10, 2, true);
    ASSERT_EQ_INT(xr_range_core_length(forward), 5);
    ASSERT_TRUE(xr_range_core_contains(forward, 9));
    ASSERT_FALSE(xr_range_core_contains(forward, 10));

    XrRangeCore singleton = xr_range_core_make_with_bound(5, 5, 1, true);
    ASSERT_EQ_INT(xr_range_core_length(singleton), 1);
    ASSERT_TRUE(xr_range_core_contains(singleton, 5));

    XrRangeCore reverse = xr_range_core_make_with_bound(10, 1, -3, true);
    ASSERT_EQ_INT(xr_range_core_length(reverse), 4);
    ASSERT_TRUE(xr_range_core_contains(reverse, 1));
}

TEST(range_core_contains_respects_step_and_exclusive_end) {
    XrRangeCore forward = xr_range_core_make(1, 10, 2);
    ASSERT_TRUE(xr_range_core_contains(forward, 1));
    ASSERT_TRUE(xr_range_core_contains(forward, 9));
    ASSERT_FALSE(xr_range_core_contains(forward, 10));
    ASSERT_FALSE(xr_range_core_contains(forward, 8));

    XrRangeCore reverse = xr_range_core_make(10, 1, -3);
    ASSERT_TRUE(xr_range_core_contains(reverse, 10));
    ASSERT_TRUE(xr_range_core_contains(reverse, 4));
    ASSERT_FALSE(xr_range_core_contains(reverse, 1));
    ASSERT_FALSE(xr_range_core_contains(reverse, 5));
}

TEST(range_core_index_rejects_negative_indices) {
    bool ok = false;
    XrRangeCore forward = xr_range_core_make(2, 9, 2);
    ASSERT_EQ_INT(xr_range_core_index(forward, 0, &ok), 2);
    ASSERT_TRUE(ok);
    ASSERT_EQ_INT(xr_range_core_index(forward, 3, &ok), 8);
    ASSERT_TRUE(ok);
    ASSERT_EQ_INT(xr_range_core_index(forward, -1, &ok), 0);
    ASSERT_FALSE(ok);
    ASSERT_EQ_INT(xr_range_core_index(forward, -4, &ok), 0);
    ASSERT_FALSE(ok);
    ASSERT_EQ_INT(xr_range_core_index(forward, -5, &ok), 0);
    ASSERT_FALSE(ok);

    XrRangeCore reverse = xr_range_core_make(10, 1, -3);
    ASSERT_EQ_INT(xr_range_core_index(reverse, 0, &ok), 10);
    ASSERT_TRUE(ok);
    ASSERT_EQ_INT(xr_range_core_index(reverse, -1, &ok), 0);
    ASSERT_FALSE(ok);
}

TEST(range_owner_apply_covers_boundaries) {
    XrRangeCore half_open = XR_RANGE_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_RANGE_HI, XR_SEM_OWNER_ID_SHARED_RANGE_LO,
        XR_SEM_CONSUMER_VM, INT64_MIN, INT64_MAX, false);
    ASSERT_EQ_INT(half_open.start, INT64_MIN);
    ASSERT_EQ_INT(half_open.end, INT64_MAX);
    ASSERT_EQ_INT(half_open.step, 1);
    ASSERT_FALSE(half_open.inclusive_end);
    ASSERT_EQ_INT(xr_range_core_length(half_open), INT64_MAX);
    ASSERT_TRUE(xr_range_core_contains(half_open, INT64_MAX - 1));
    ASSERT_FALSE(xr_range_core_contains(half_open, INT64_MAX));

    XrRangeCore inclusive = XR_RANGE_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_RANGE_HI, XR_SEM_OWNER_ID_SHARED_RANGE_LO,
        XR_SEM_CONSUMER_AOT_HOSTED, INT64_MAX - 1, INT64_MAX, true);
    ASSERT_EQ_INT(xr_range_core_length(inclusive), 2);
    ASSERT_TRUE(xr_range_core_contains(inclusive, INT64_MAX));
}

TEST(range_core_materialize_plan_caps_large_ranges) {
    XrRangeCoreMaterializePlan empty = xr_range_core_materialize_plan(xr_range_core_make(5, 5, 1));
    ASSERT_EQ_INT(empty.kind, XR_RANGE_CORE_MATERIALIZE_EMPTY);
    ASSERT_EQ_INT(empty.length, 0);

    XrRangeCoreMaterializePlan values = xr_range_core_materialize_plan(xr_range_core_make(2, 9, 2));
    ASSERT_EQ_INT(values.kind, XR_RANGE_CORE_MATERIALIZE_VALUES);
    ASSERT_EQ_INT(values.length, 4);
    ASSERT_EQ_INT(xr_range_core_value_at(xr_range_core_make(2, 9, 2), values.length - 1), 8);

    XrRangeCoreMaterializePlan reverse =
        xr_range_core_materialize_plan(xr_range_core_make(10, 1, -3));
    ASSERT_EQ_INT(reverse.kind, XR_RANGE_CORE_MATERIALIZE_VALUES);
    ASSERT_EQ_INT(reverse.length, 3);
    ASSERT_EQ_INT(xr_range_core_value_at(xr_range_core_make(10, 1, -3), reverse.length - 1), 4);

    XrRangeCore inclusive = xr_range_core_make_with_bound(1, 3, 1, true);
    XrRangeCoreMaterializePlan inclusive_values = xr_range_core_materialize_plan(inclusive);
    ASSERT_EQ_INT(inclusive_values.kind, XR_RANGE_CORE_MATERIALIZE_VALUES);
    ASSERT_EQ_INT(inclusive_values.length, 3);
    ASSERT_EQ_INT(xr_range_core_value_at(inclusive, inclusive_values.length - 1), 3);

    XrRangeCoreMaterializePlan too_large =
        xr_range_core_materialize_plan(xr_range_core_make(0, XR_RANGE_CORE_MATERIALIZE_MAX + 1, 1));
    ASSERT_EQ_INT(too_large.kind, XR_RANGE_CORE_MATERIALIZE_TOO_LARGE);
    ASSERT_EQ_INT(too_large.length, XR_RANGE_CORE_MATERIALIZE_MAX + 1);
}

TEST(range_core_format_matches_range_to_string) {
    char buf[64];
    ASSERT_EQ_INT(xr_range_core_format_buf(xr_range_core_make(1, 10, 1), buf, sizeof(buf)), 5);
    ASSERT_STR_EQ(buf, "1..10");
    ASSERT_EQ_INT(xr_range_core_format_buf(xr_range_core_make(10, 1, -2), buf, sizeof(buf)), 8);
    ASSERT_STR_EQ(buf, "10..1:-2");
    ASSERT_EQ_INT(
        xr_range_core_format_buf(xr_range_core_make_with_bound(1, 3, 1, true), buf, sizeof(buf)),
        5);
    ASSERT_STR_EQ(buf, "1..=3");
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Range Core");
RUN_TEST(range_core_length_uses_half_open_forward_bounds);
RUN_TEST(range_core_length_uses_half_open_reverse_bounds);
RUN_TEST(range_core_length_and_contains_use_inclusive_bounds);
RUN_TEST(range_core_contains_respects_step_and_exclusive_end);
RUN_TEST(range_core_index_rejects_negative_indices);
RUN_TEST(range_owner_apply_covers_boundaries);
RUN_TEST(range_core_materialize_plan_caps_large_ranges);
RUN_TEST(range_core_format_matches_range_to_string);

TEST_MAIN_END()
