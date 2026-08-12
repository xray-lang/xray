/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xrt_byte_slice_compare_owner_freestanding.c - freestanding byte compare owner KAT
 */

#include "../test_framework.h"
#include "aot/xrt_core_freestanding.h"

TEST(freestanding_byte_slice_compare_owner_preserves_unsigned_and_prefix_ordering) {
    uint8_t left_bytes[] = {0, 127, 128, 255};
    uint8_t lower_bytes[] = {0, 127, 128, 254};
    xr_span_t left = {left_bytes, 4};
    xr_span_t equal = {left_bytes, 4};
    xr_span_t lower = {lower_bytes, 4};
    xr_span_t prefix = {left_bytes, 3};
    xr_span_t empty = {NULL, 0};

    ASSERT_EQ_INT(xrt_byte_slice_compare_checked_raw(left, equal), 0);
    ASSERT_EQ_INT(xrt_byte_slice_compare_checked_raw(lower, left), -1);
    ASSERT_EQ_INT(xrt_byte_slice_compare_checked_raw(left, lower), 1);
    ASSERT_EQ_INT(xrt_byte_slice_compare_checked_raw(prefix, left), -1);
    ASSERT_EQ_INT(xrt_byte_slice_compare_checked_raw(left, prefix), 1);
    ASSERT_EQ_INT(xrt_byte_slice_compare_checked_raw(empty, empty), 0);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Freestanding Byte Slice Compare Owner");
RUN_TEST(freestanding_byte_slice_compare_owner_preserves_unsigned_and_prefix_ordering);
TEST_MAIN_END()
