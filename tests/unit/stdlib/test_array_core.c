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

TEST(array_core_fill_range_matches_slice_bounds) {
    assert_range(xr_array_core_fill_range(6, 1, 4), 1, 4, 3);
    assert_range(xr_array_core_fill_range(6, -3, -1), 3, 5, 2);
    assert_range(xr_array_core_fill_range(6, 4, 2), 2, 2, 0);
}

TEST(array_core_typed_index_of_matches_boxed_tags) {
    int handled = 0;
    uint8_t bytes[] = {1, 255, 3};
    ASSERT_EQ_INT(
        xr_array_core_typed_index_of(bytes, 3, XR_ELEM_U8, xr_array_core_needle_int(255), &handled),
        1);
    ASSERT_TRUE(handled);
    ASSERT_EQ_INT(
        xr_array_core_typed_index_of(bytes, 3, XR_ELEM_U8, xr_array_core_needle_int(-1), &handled),
        -1);
    ASSERT_EQ_INT(xr_array_core_typed_index_of(bytes, 3, XR_ELEM_U8,
                                               xr_array_core_needle_bool(true), &handled),
                  -1);

    uint8_t bools[] = {0, 1, 0};
    ASSERT_EQ_INT(xr_array_core_typed_index_of(bools, 3, XR_ELEM_BOOL,
                                               xr_array_core_needle_bool(true), &handled),
                  1);
    ASSERT_EQ_INT(
        xr_array_core_typed_index_of(bools, 3, XR_ELEM_BOOL, xr_array_core_needle_int(1), &handled),
        -1);

    float floats[] = {1.25f, 2.5f};
    ASSERT_EQ_INT(xr_array_core_typed_index_of(floats, 2, XR_ELEM_F32,
                                               xr_array_core_needle_float(2.5), &handled),
                  1);
    ASSERT_EQ_INT(
        xr_array_core_typed_index_of(floats, 2, XR_ELEM_F32, xr_array_core_needle_int(2), &handled),
        -1);

    ASSERT_EQ_INT(
        xr_array_core_typed_index_of(bytes, 3, XR_ELEM_ANY, xr_array_core_needle_int(1), &handled),
        -1);
    ASSERT_FALSE(handled);
}

TEST(array_core_bytes_loads_little_endian_and_rejects_invalid_ranges) {
    uint8_t bytes[] = {1, 2, 3, 4, 5, 6, 7, 8};
    bool ok = false;

    ASSERT_TRUE(xr_array_core_bytes_range_ok(8, XR_ELEM_U8, 0, 8));
    ASSERT_FALSE(xr_array_core_bytes_range_ok(8, XR_ELEM_I64, 0, 8));
    ASSERT_FALSE(xr_array_core_bytes_range_ok(8, XR_ELEM_U8, -1, 1));
    ASSERT_FALSE(xr_array_core_bytes_range_ok(8, XR_ELEM_U8, 5, 4));

    ASSERT_EQ_UINT(xr_array_core_bytes_load_u32_le(bytes, 8, XR_ELEM_U8, 0, &ok), 67305985u);
    ASSERT_TRUE(ok);
    ASSERT_EQ_UINT(xr_array_core_bytes_load_u64_le(bytes, 8, XR_ELEM_U8, 0, &ok),
                   UINT64_C(578437695752307201));
    ASSERT_TRUE(ok);
    ASSERT_EQ_UINT(xr_array_core_bytes_load_u32_le(bytes, 8, XR_ELEM_U8, 5, &ok), 0u);
    ASSERT_FALSE(ok);
    ASSERT_EQ_UINT(xr_array_core_bytes_load_u32_le(NULL, 8, XR_ELEM_U8, 0, &ok), 0u);
    ASSERT_FALSE(ok);
}

TEST(array_core_bytes_copy_uses_shared_range_and_overlap_rules) {
    uint8_t data[] = {1, 2, 3, 4, 5, 6};
    ASSERT_TRUE(xr_array_core_bytes_copy_within(data, 6, XR_ELEM_U8, 2, 0, 4));
    ASSERT_EQ_INT(data[0], 1);
    ASSERT_EQ_INT(data[2], 1);
    ASSERT_EQ_INT(data[3], 2);
    ASSERT_EQ_INT(data[5], 4);
    ASSERT_FALSE(xr_array_core_bytes_copy_within(data, 6, XR_ELEM_U8, 3, 0, 4));

    uint8_t src[] = {9, 8, 7, 6};
    uint8_t dst[] = {0, 0, 0, 0, 0, 0};
    ASSERT_TRUE(
        xr_array_core_bytes_copy_from(dst, 6, XR_ELEM_U8, src, 4, XR_ELEM_U8, 1, 2, 3, false));
    ASSERT_EQ_INT(dst[2], 8);
    ASSERT_EQ_INT(dst[3], 7);
    ASSERT_EQ_INT(dst[4], 6);

    uint8_t overlap[] = {10, 20, 30, 40, 50, 60};
    ASSERT_TRUE(xr_array_core_bytes_copy_from(overlap, 6, XR_ELEM_U8, overlap, 6, XR_ELEM_U8, 0, 2,
                                              4, true));
    ASSERT_EQ_INT(overlap[2], 10);
    ASSERT_EQ_INT(overlap[3], 20);
    ASSERT_EQ_INT(overlap[4], 30);
    ASSERT_EQ_INT(overlap[5], 40);
    ASSERT_FALSE(
        xr_array_core_bytes_copy_from(dst, 6, XR_ELEM_U8, src, 4, XR_ELEM_I64, 0, 0, 1, false));
}

TEST(array_core_bytes_repeat_from_matches_lz_style_overlap) {
    uint8_t rep[9] = {65, 66, 67, 0, 0, 0, 0, 0, 0};
    ASSERT_TRUE(xr_array_core_bytes_repeat_from(rep, 9, XR_ELEM_U8, 3, 3, 6));
    ASSERT_EQ_INT(rep[3], 65);
    ASSERT_EQ_INT(rep[4], 66);
    ASSERT_EQ_INT(rep[5], 67);
    ASSERT_EQ_INT(rep[6], 65);
    ASSERT_EQ_INT(rep[7], 66);
    ASSERT_EQ_INT(rep[8], 67);

    uint8_t empty_count[] = {1, 2};
    ASSERT_TRUE(xr_array_core_bytes_repeat_from(empty_count, 2, XR_ELEM_U8, 1, 1, 0));
    ASSERT_EQ_INT(empty_count[0], 1);
    ASSERT_EQ_INT(empty_count[1], 2);
    ASSERT_FALSE(xr_array_core_bytes_repeat_from(empty_count, 2, XR_ELEM_U8, 0, 1, 1));
    ASSERT_FALSE(xr_array_core_bytes_repeat_from(empty_count, 2, XR_ELEM_U8, 1, 0, 1));
    ASSERT_FALSE(xr_array_core_bytes_repeat_from(empty_count, 2, XR_ELEM_I64, 1, 1, 0));
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Array Core - Slice Range");
RUN_TEST(array_core_slice_range_clamps_positive_bounds);
RUN_TEST(array_core_slice_range_handles_negative_bounds);
RUN_TEST(array_core_slice_range_empty_when_start_after_end);
RUN_TEST(array_core_slice_range_accepts_nonpositive_length);
RUN_TEST(array_core_fill_range_matches_slice_bounds);
RUN_TEST(array_core_typed_index_of_matches_boxed_tags);
RUN_TEST(array_core_bytes_loads_little_endian_and_rejects_invalid_ranges);
RUN_TEST(array_core_bytes_copy_uses_shared_range_and_overlap_rules);
RUN_TEST(array_core_bytes_repeat_from_matches_lz_style_overlap);

TEST_MAIN_END()
