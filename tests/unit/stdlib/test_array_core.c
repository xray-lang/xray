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
#include "runtime/value/xvalue.h"
#include "shared/xr_array_core.h"
#include "shared/xr_sort_core.h"

static void assert_range(XrArrayCoreRange range, int64_t start, int64_t end, int64_t count) {
    ASSERT_EQ_INT(range.start, start);
    ASSERT_EQ_INT(range.end, end);
    ASSERT_EQ_INT(range.count, count);
}

typedef struct JoinParts {
    const char **parts;
} JoinParts;

typedef struct ArrayHofTestCtx {
    XrValue input[8];
    XrValue output[8];
    int64_t read_order[8];
    int64_t write_order[8];
    int read_count;
    int write_count;
} ArrayHofTestCtx;

static bool join_parts_element(void *ctx, int64_t index, char *dst, size_t *len) {
    JoinParts *parts = (JoinParts *) ctx;
    if (!parts || index < 0)
        return false;
    const char *part = parts->parts[index];
    if (!part)
        return false;
    size_t n = strlen(part);
    if (dst && n > 0)
        memcpy(dst, part, n);
    if (len)
        *len = n;
    return true;
}

static XrValue array_hof_test_read(void *ctx_ptr, int64_t index) {
    ArrayHofTestCtx *ctx = (ArrayHofTestCtx *) ctx_ptr;
    ctx->read_order[ctx->read_count++] = index;
    return ctx->input[index];
}

static XrValue array_hof_test_map_double(void *ctx_ptr, XrValue value) {
    (void) ctx_ptr;
    return XR_FROM_INT(XR_TO_INT(value) * 2);
}

static bool array_hof_test_write(void *ctx_ptr, int64_t index, XrValue value) {
    ArrayHofTestCtx *ctx = (ArrayHofTestCtx *) ctx_ptr;
    ctx->write_order[ctx->write_count++] = index;
    ctx->output[index] = value;
    return true;
}

static bool array_hof_test_predicate_odd(void *ctx_ptr, XrValue value) {
    (void) ctx_ptr;
    return (XR_TO_INT(value) & 1) != 0;
}

static XrValue array_hof_test_reduce_digits(void *ctx_ptr, XrValue acc, XrValue value) {
    (void) ctx_ptr;
    return XR_FROM_INT(XR_TO_INT(acc) * 10 + XR_TO_INT(value));
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

TEST(array_core_join_plans_total_and_writes_separators) {
    const char *items[] = {"aa", "b", ""};
    JoinParts ctx = {items};
    size_t total = 0;
    ASSERT_TRUE(xr_array_core_join_total(3, 2, join_parts_element, &ctx, &total));
    ASSERT_EQ_INT(total, 7);

    char out[8];
    size_t written = 0;
    ASSERT_TRUE(
        xr_array_core_join_write(out, sizeof(out), 3, "::", 2, join_parts_element, &ctx, &written));
    ASSERT_EQ_INT(written, 7);
    ASSERT_STR_EQ(out, "aa::b::");
}

TEST(array_core_join_handles_empty_and_invalid_capacity) {
    const char *items[] = {"x", "y"};
    JoinParts ctx = {items};
    size_t total = 99;
    ASSERT_TRUE(xr_array_core_join_total(0, 1, join_parts_element, &ctx, &total));
    ASSERT_EQ_INT(total, 0);

    char out[2] = {'?', '?'};
    ASSERT_TRUE(
        xr_array_core_join_write(out, sizeof(out), 0, ",", 1, join_parts_element, &ctx, NULL));
    ASSERT_STR_EQ(out, "");

    ASSERT_FALSE(xr_array_core_join_write(out, 3, 2, ",", 1, join_parts_element, &ctx, NULL));
}

TEST(array_core_hof_map_preserves_iteration_and_write_indices) {
    ArrayHofTestCtx ctx = {0};
    ctx.input[0] = XR_FROM_INT(2);
    ctx.input[1] = XR_FROM_INT(3);
    ctx.input[2] = XR_FROM_INT(4);

    int64_t count = -1;
    ASSERT_TRUE(xr_array_core_hof_map(3, array_hof_test_read, array_hof_test_map_double,
                                      array_hof_test_write, &ctx, &count));
    ASSERT_EQ_INT(count, 3);
    ASSERT_EQ_INT(ctx.read_count, 3);
    ASSERT_EQ_INT(ctx.write_count, 3);
    ASSERT_EQ_INT(ctx.read_order[0], 0);
    ASSERT_EQ_INT(ctx.read_order[2], 2);
    ASSERT_EQ_INT(ctx.write_order[0], 0);
    ASSERT_EQ_INT(ctx.write_order[2], 2);
    ASSERT_EQ_INT(XR_TO_INT(ctx.output[0]), 4);
    ASSERT_EQ_INT(XR_TO_INT(ctx.output[1]), 6);
    ASSERT_EQ_INT(XR_TO_INT(ctx.output[2]), 8);
}

TEST(array_core_hof_filter_compacts_kept_values) {
    ArrayHofTestCtx ctx = {0};
    for (int i = 0; i < 5; i++)
        ctx.input[i] = XR_FROM_INT(i + 1);

    int64_t count = -1;
    ASSERT_TRUE(xr_array_core_hof_filter(5, array_hof_test_read, array_hof_test_predicate_odd,
                                         array_hof_test_write, &ctx, &count));
    ASSERT_EQ_INT(count, 3);
    ASSERT_EQ_INT(ctx.read_count, 5);
    ASSERT_EQ_INT(ctx.write_count, 3);
    ASSERT_EQ_INT(ctx.write_order[0], 0);
    ASSERT_EQ_INT(ctx.write_order[1], 1);
    ASSERT_EQ_INT(ctx.write_order[2], 2);
    ASSERT_EQ_INT(XR_TO_INT(ctx.output[0]), 1);
    ASSERT_EQ_INT(XR_TO_INT(ctx.output[1]), 3);
    ASSERT_EQ_INT(XR_TO_INT(ctx.output[2]), 5);
}

TEST(array_core_hof_reduce_updates_accumulator_in_order) {
    ArrayHofTestCtx ctx = {0};
    ctx.input[0] = XR_FROM_INT(1);
    ctx.input[1] = XR_FROM_INT(2);
    ctx.input[2] = XR_FROM_INT(3);

    XrValue result = xr_array_core_hof_reduce(3, array_hof_test_read, array_hof_test_reduce_digits,
                                              &ctx, XR_FROM_INT(0));
    ASSERT_EQ_INT(XR_TO_INT(result), 123);
    ASSERT_EQ_INT(ctx.read_count, 3);
    ASSERT_EQ_INT(ctx.read_order[0], 0);
    ASSERT_EQ_INT(ctx.read_order[2], 2);
}

TEST(array_core_hof_handles_empty_and_rejects_missing_callbacks) {
    ArrayHofTestCtx ctx = {0};
    int64_t count = 99;

    ASSERT_TRUE(xr_array_core_hof_map(0, NULL, NULL, NULL, &ctx, &count));
    ASSERT_EQ_INT(count, 0);
    ASSERT_FALSE(xr_array_core_hof_map(1, NULL, array_hof_test_map_double, array_hof_test_write,
                                       &ctx, &count));
    ASSERT_FALSE(
        xr_array_core_hof_filter(1, array_hof_test_read, NULL, array_hof_test_write, &ctx, &count));
    XrValue initial = XR_FROM_INT(42);
    XrValue reduced =
        xr_array_core_hof_reduce(1, NULL, array_hof_test_reduce_digits, &ctx, initial);
    ASSERT_EQ_INT(XR_TO_INT(reduced), 42);
}

TEST(array_core_fill_typed_storage_coerces_once_and_fills_range) {
    int64_t ints[] = {0, 0, 0, 0, 0};
    ASSERT_TRUE(xr_array_core_fill_typed_storage(ints, 1, 3, XR_ELEM_I64, XR_FROM_FLOAT(7.9)));
    ASSERT_EQ_INT(ints[0], 0);
    ASSERT_EQ_INT(ints[1], 7);
    ASSERT_EQ_INT(ints[3], 7);
    ASSERT_EQ_INT(ints[4], 0);

    uint8_t bytes[] = {1, 2, 3, 4};
    ASSERT_TRUE(xr_array_core_fill_typed_storage(bytes, 1, 2, XR_ELEM_U8, XR_FROM_INT(255)));
    ASSERT_EQ_INT(bytes[0], 1);
    ASSERT_EQ_INT(bytes[1], 255);
    ASSERT_EQ_INT(bytes[2], 255);
    ASSERT_EQ_INT(bytes[3], 4);

    double doubles[] = {0.0, 0.0, 0.0};
    ASSERT_TRUE(xr_array_core_fill_typed_storage(doubles, 0, 3, XR_ELEM_F64, XR_FROM_INT(5)));
    ASSERT(doubles[0] == 5.0);
    ASSERT(doubles[2] == 5.0);
}

TEST(array_core_fill_typed_storage_handles_bool_and_invalid_cases) {
    uint8_t bools[] = {1, 1, 1, 1};
    ASSERT_TRUE(xr_array_core_fill_typed_storage(bools, 0, 4, XR_ELEM_BOOL, XR_NULL_VAL));
    ASSERT_EQ_INT(bools[0], 0);
    ASSERT_EQ_INT(bools[3], 0);

    ASSERT_TRUE(xr_array_core_fill_typed_storage(bools, 1, 2, XR_ELEM_BOOL, XR_FROM_INT(9)));
    ASSERT_EQ_INT(bools[0], 0);
    ASSERT_EQ_INT(bools[1], 1);
    ASSERT_EQ_INT(bools[2], 1);
    ASSERT_EQ_INT(bools[3], 0);

    XrValue boxed[] = {XR_NULL_VAL, XR_NULL_VAL};
    ASSERT_FALSE(xr_array_core_fill_typed_storage(boxed, 0, 2, XR_ELEM_ANY, XR_FROM_INT(1)));
    ASSERT_TRUE(xr_array_core_fill_typed_storage(NULL, 0, 0, XR_ELEM_U8, XR_FROM_INT(1)));
    ASSERT_FALSE(xr_array_core_fill_typed_storage(NULL, 0, 1, XR_ELEM_U8, XR_FROM_INT(1)));
    ASSERT_FALSE(xr_array_core_fill_typed_storage(bools, -1, 1, XR_ELEM_BOOL, XR_TRUE_VAL));
}

TEST(array_core_index_set_plan_rejects_wraparound_and_gaps) {
    XrArrayCoreIndexSetPlan plan = xr_array_core_index_set_plan(3, 1);
    ASSERT_EQ_INT(plan.kind, XR_ARRAY_CORE_INDEX_SET_WRITE);
    ASSERT_EQ_INT(plan.index, 1);

    plan = xr_array_core_index_set_plan(3, -1);
    ASSERT_EQ_INT(plan.kind, XR_ARRAY_CORE_INDEX_SET_INVALID);

    plan = xr_array_core_index_set_plan(3, 5);
    ASSERT_EQ_INT(plan.kind, XR_ARRAY_CORE_INDEX_SET_INVALID);

    plan = xr_array_core_index_set_plan(3, 3);
    ASSERT_EQ_INT(plan.kind, XR_ARRAY_CORE_INDEX_SET_INVALID);
}

TEST(array_core_reverse_swaps_raw_elements_by_size) {
    uint8_t bytes[] = {1, 2, 3, 4};
    ASSERT_TRUE(xr_array_core_reverse(bytes, 4, 1));
    ASSERT_EQ_INT(bytes[0], 4);
    ASSERT_EQ_INT(bytes[1], 3);
    ASSERT_EQ_INT(bytes[2], 2);
    ASSERT_EQ_INT(bytes[3], 1);

    int64_t ints[] = {10, 20, 30};
    ASSERT_TRUE(xr_array_core_reverse(ints, 3, 8));
    ASSERT_EQ_INT(ints[0], 30);
    ASSERT_EQ_INT(ints[1], 20);
    ASSERT_EQ_INT(ints[2], 10);

    XrValue boxed[] = {XR_FROM_INT(7), XR_FROM_FLOAT(2.5), XR_FROM_BOOL(true)};
    ASSERT_TRUE(xr_array_core_reverse(boxed, 3, sizeof(XrValue)));
    ASSERT_TRUE(XR_IS_BOOL(boxed[0]));
    ASSERT_TRUE(XR_IS_FLOAT(boxed[1]));
    ASSERT_TRUE(XR_IS_INT(boxed[2]));
    ASSERT_EQ_INT(XR_TO_INT(boxed[2]), 7);
}

TEST(array_core_reverse_handles_empty_single_and_invalid_data) {
    uint8_t value = 42;
    ASSERT_TRUE(xr_array_core_reverse(NULL, 0, 1));
    ASSERT_TRUE(xr_array_core_reverse(&value, 1, 1));
    ASSERT_EQ_INT(value, 42);
    ASSERT_FALSE(xr_array_core_reverse(NULL, 2, 1));
    ASSERT_FALSE(xr_array_core_reverse(&value, 2, 0));
}

TEST(array_core_shift_left_one_moves_raw_elements_by_size) {
    uint8_t bytes[] = {1, 2, 3, 4};
    ASSERT_TRUE(xr_array_core_shift_left_one(bytes, 4, 1));
    ASSERT_EQ_INT(bytes[0], 2);
    ASSERT_EQ_INT(bytes[1], 3);
    ASSERT_EQ_INT(bytes[2], 4);

    int64_t ints[] = {10, 20, 30};
    ASSERT_TRUE(xr_array_core_shift_left_one(ints, 3, 8));
    ASSERT_EQ_INT(ints[0], 20);
    ASSERT_EQ_INT(ints[1], 30);

    XrValue boxed[] = {XR_FROM_INT(7), XR_FROM_FLOAT(2.5), XR_FROM_BOOL(true)};
    ASSERT_TRUE(xr_array_core_shift_left_one(boxed, 3, sizeof(XrValue)));
    ASSERT_TRUE(XR_IS_FLOAT(boxed[0]));
    ASSERT_TRUE(XR_IS_BOOL(boxed[1]));
    ASSERT_EQ_INT(XR_TO_BOOL(boxed[1]), true);
}

TEST(array_core_shift_left_one_handles_empty_single_and_invalid_data) {
    uint8_t value = 42;
    ASSERT_TRUE(xr_array_core_shift_left_one(NULL, 0, 1));
    ASSERT_TRUE(xr_array_core_shift_left_one(&value, 1, 1));
    ASSERT_EQ_INT(value, 42);
    ASSERT_FALSE(xr_array_core_shift_left_one(NULL, 2, 1));
    ASSERT_FALSE(xr_array_core_shift_left_one(&value, 2, 0));
}

TEST(array_core_shift_right_one_moves_raw_elements_by_size) {
    uint8_t bytes[5] = {1, 2, 3, 4, 0};
    ASSERT_TRUE(xr_array_core_shift_right_one(bytes, 4, 1));
    ASSERT_EQ_INT(bytes[1], 1);
    ASSERT_EQ_INT(bytes[2], 2);
    ASSERT_EQ_INT(bytes[3], 3);
    ASSERT_EQ_INT(bytes[4], 4);

    int64_t ints[4] = {10, 20, 30, 0};
    ASSERT_TRUE(xr_array_core_shift_right_one(ints, 3, 8));
    ASSERT_EQ_INT(ints[1], 10);
    ASSERT_EQ_INT(ints[2], 20);
    ASSERT_EQ_INT(ints[3], 30);

    XrValue boxed[4] = {XR_FROM_INT(7), XR_FROM_FLOAT(2.5), XR_FROM_BOOL(true), XR_NULL_VAL};
    ASSERT_TRUE(xr_array_core_shift_right_one(boxed, 3, sizeof(XrValue)));
    ASSERT_TRUE(XR_IS_INT(boxed[1]));
    ASSERT_TRUE(XR_IS_FLOAT(boxed[2]));
    ASSERT_TRUE(XR_IS_BOOL(boxed[3]));
    ASSERT_EQ_INT(XR_TO_INT(boxed[1]), 7);
}

TEST(array_core_shift_right_one_handles_empty_and_invalid_data) {
    uint8_t value = 42;
    ASSERT_TRUE(xr_array_core_shift_right_one(NULL, 0, 1));
    ASSERT_TRUE(xr_array_core_shift_right_one(&value, -1, 1));
    ASSERT_EQ_INT(value, 42);
    ASSERT_FALSE(xr_array_core_shift_right_one(NULL, 1, 1));
    ASSERT_FALSE(xr_array_core_shift_right_one(&value, 1, 0));
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

TEST(array_core_sort_typed_buffers_in_place) {
    int64_t ints[] = {3, -1, 7, 3, 0};
    ASSERT_TRUE(xr_sort_core_typed(ints, 5, XR_ELEM_I64));
    ASSERT_EQ_INT(ints[0], -1);
    ASSERT_EQ_INT(ints[1], 0);
    ASSERT_EQ_INT(ints[2], 3);
    ASSERT_EQ_INT(ints[3], 3);
    ASSERT_EQ_INT(ints[4], 7);

    uint8_t bools[] = {1, 0, 1, 0};
    ASSERT_TRUE(xr_sort_core_typed(bools, 4, XR_ELEM_BOOL));
    ASSERT_EQ_INT(bools[0], 0);
    ASSERT_EQ_INT(bools[1], 0);
    ASSERT_EQ_INT(bools[2], 1);
    ASSERT_EQ_INT(bools[3], 1);

    ASSERT_FALSE(xr_sort_core_typed(ints, 5, XR_ELEM_ANY));
}

TEST(array_core_sort_compare_result_uses_sign) {
    ASSERT_EQ_INT(xr_sort_core_compare_result(XR_FROM_INT(INT64_C(4294967296))), 1);
    ASSERT_EQ_INT(xr_sort_core_compare_result(XR_FROM_INT(-INT64_C(4294967296))), -1);
    ASSERT_EQ_INT(xr_sort_core_compare_result(XR_FROM_FLOAT(0.5)), 1);
    ASSERT_EQ_INT(xr_sort_core_compare_result(XR_FROM_FLOAT(-0.5)), -1);
    ASSERT_EQ_INT(xr_sort_core_compare_result(XR_FROM_BOOL(true)), 0);
}

TEST(array_core_sort_default_compare_numbers_and_string_slices) {
    ASSERT_LT(xr_sort_core_compare_default(XR_FROM_INT(3), XR_FROM_FLOAT(4.0), NULL, 0, NULL, 0),
              0);
    ASSERT_GT(xr_sort_core_compare_default(XR_FROM_FLOAT(5.5), XR_FROM_INT(5), NULL, 0, NULL, 0),
              0);
    ASSERT_LT(xr_sort_core_compare_default(XR_NULL_VAL, XR_NULL_VAL, "aa", 2, "b", 1), 0);
    ASSERT_GT(xr_sort_core_compare_default(XR_NULL_VAL, XR_NULL_VAL, "abc", 3, "ab", 2), 0);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Array Core - Slice Range");
RUN_TEST(array_core_slice_range_clamps_positive_bounds);
RUN_TEST(array_core_slice_range_handles_negative_bounds);
RUN_TEST(array_core_slice_range_empty_when_start_after_end);
RUN_TEST(array_core_slice_range_accepts_nonpositive_length);
RUN_TEST(array_core_fill_range_matches_slice_bounds);
RUN_TEST(array_core_join_plans_total_and_writes_separators);
RUN_TEST(array_core_join_handles_empty_and_invalid_capacity);
RUN_TEST(array_core_hof_map_preserves_iteration_and_write_indices);
RUN_TEST(array_core_hof_filter_compacts_kept_values);
RUN_TEST(array_core_hof_reduce_updates_accumulator_in_order);
RUN_TEST(array_core_hof_handles_empty_and_rejects_missing_callbacks);
RUN_TEST(array_core_fill_typed_storage_coerces_once_and_fills_range);
RUN_TEST(array_core_fill_typed_storage_handles_bool_and_invalid_cases);
RUN_TEST(array_core_index_set_plan_rejects_wraparound_and_gaps);
RUN_TEST(array_core_reverse_swaps_raw_elements_by_size);
RUN_TEST(array_core_reverse_handles_empty_single_and_invalid_data);
RUN_TEST(array_core_shift_left_one_moves_raw_elements_by_size);
RUN_TEST(array_core_shift_left_one_handles_empty_single_and_invalid_data);
RUN_TEST(array_core_shift_right_one_moves_raw_elements_by_size);
RUN_TEST(array_core_shift_right_one_handles_empty_and_invalid_data);
RUN_TEST(array_core_typed_index_of_matches_boxed_tags);
RUN_TEST(array_core_bytes_loads_little_endian_and_rejects_invalid_ranges);
RUN_TEST(array_core_bytes_copy_uses_shared_range_and_overlap_rules);
RUN_TEST(array_core_bytes_repeat_from_matches_lz_style_overlap);
RUN_TEST(array_core_sort_typed_buffers_in_place);
RUN_TEST(array_core_sort_compare_result_uses_sign);
RUN_TEST(array_core_sort_default_compare_numbers_and_string_slices);

TEST_MAIN_END()
