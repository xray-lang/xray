/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_array_core.c - Unit tests for runtime-neutral Array core helpers
 */

#include "../test_framework.h"
#include "shared/xr_array_core.h"

static void assert_range(XrArrayCoreRange range, int64_t start, int64_t end, int64_t count) {
    ASSERT_EQ_INT(range.start, start);
    ASSERT_EQ_INT(range.end, end);
    ASSERT_EQ_INT(range.count, count);
}

TEST(array_core_slice_range_clamps_positive_bounds) {
    assert_range(xr_array_core_slice_range(5, 1, 4), 1, 4, 3);
    assert_range(xr_array_core_slice_range(5, 0, 99), 0, 5, 5);
    assert_range(xr_array_core_slice_range(5, 99, 100), 5, 5, 0);
}

TEST(array_core_slice_range_handles_negative_bounds) {
    assert_range(xr_array_core_slice_range(5, -4, -1), 1, 4, 3);
    assert_range(xr_array_core_slice_range(5, -99, 3), 0, 3, 3);
    assert_range(xr_array_core_slice_range(5, 2, -99), 0, 0, 0);
}

TEST(array_core_slice_range_empty_when_start_after_end) {
    assert_range(xr_array_core_slice_range(5, 4, 2), 2, 2, 0);
    assert_range(xr_array_core_slice_range(5, -1, -3), 2, 2, 0);
}

TEST(array_core_slice_range_accepts_nonpositive_length) {
    assert_range(xr_array_core_slice_range(0, -1, 10), 0, 0, 0);
    assert_range(xr_array_core_slice_range(-7, -1, 10), 0, 0, 0);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Array Core - Slice Range");
RUN_TEST(array_core_slice_range_clamps_positive_bounds);
RUN_TEST(array_core_slice_range_handles_negative_bounds);
RUN_TEST(array_core_slice_range_empty_when_start_after_end);
RUN_TEST(array_core_slice_range_accepts_nonpositive_length);

TEST_MAIN_END()
