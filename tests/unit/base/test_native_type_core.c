/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_type_core.c - Unit tests for shared native field layout rules
 */

#include "../test_framework.h"
#include "runtime/value/xstruct_layout.h"
#include "shared/xr_native_type_core.h"

#include <string.h>

static XrAggregateLayout aggregate(uint8_t kind, uint16_t field_count) {
    XrAggregateLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.kind = kind;
    layout.field_count = field_count;
    return layout;
}

TEST(native_type_tag_values_are_stable) {
    ASSERT_EQ_INT(XR_NATIVE_I64, 0);
    ASSERT_EQ_INT(XR_NATIVE_F64, 1);
    ASSERT_EQ_INT(XR_NATIVE_BOOL, 2);
    ASSERT_EQ_INT(XR_NATIVE_ARRAY_REF, 14);
    ASSERT_EQ_INT(XR_NATIVE_MAP_REF, 15);
    ASSERT_EQ_INT(XR_NATIVE_SET_REF, 16);
    ASSERT_EQ_INT(XR_NATIVE_VALUE, 17);
}

TEST(native_type_scalar_sizes_are_stable) {
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_I8), 1);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_U8), 1);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_BOOL), 1);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_I16), 2);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_U16), 2);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_I32), 4);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_U32), 4);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_F32), 4);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_I64), 8);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_U64), 8);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_F64), 8);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_STRING), 8);
}

TEST(native_type_aggregate_sizes_are_stable) {
    const XrTargetDataLayout *host = xr_target_data_layout_host();
    ASSERT_EQ_INT(xr_native_type_size(host, XR_NATIVE_NESTED_AGGREGATE), 0);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_ARRAY), 0);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_ARRAY_REF), 16);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_MAP_REF), 16);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_SET_REF), 16);
    ASSERT_EQ_INT(xr_native_type_size(xr_target_data_layout_host(), XR_NATIVE_VALUE), 16);
}

TEST(target_data_layout_distinguishes_ilp32_and_lp64) {
    XrTargetDataLayout ilp32;
    XrTargetDataLayout lp64;
    ASSERT_TRUE(xr_target_data_layout_init_ilp32(&ilp32));
    ASSERT_TRUE(xr_target_data_layout_init_lp64(&lp64));
    ASSERT_TRUE(xr_target_data_layout_validate(&ilp32));
    ASSERT_TRUE(xr_target_data_layout_validate(&lp64));
    ASSERT_EQ_UINT(ilp32.pointer.size, 4);
    ASSERT_EQ_UINT(ilp32.isize.size, 4);
    ASSERT_EQ_UINT(ilp32.usize.size, 4);
    ASSERT_EQ_UINT(lp64.pointer.size, 8);
    ASSERT_EQ_UINT(lp64.isize.size, 8);
    ASSERT_EQ_UINT(lp64.usize.size, 8);
    ASSERT_NE(ilp32.stable_hash, lp64.stable_hash);
}

TEST(target_layout_query_owner_covers_width_and_failure_edges) {
    XrTargetDataLayout ilp32;
    XrTargetDataLayout lp64;
    ASSERT_TRUE(xr_target_data_layout_init_ilp32(&ilp32));
    ASSERT_TRUE(xr_target_data_layout_init_lp64(&lp64));

    XrTargetLayoutQueryResult result = XR_TARGET_LAYOUT_QUERY_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_TARGET_LAYOUT_QUERY_HI,
        XR_SEM_OWNER_ID_SHARED_TARGET_LAYOUT_QUERY_LO, XR_SEM_CONSUMER_VM,
        xr_target_layout_query_core(XR_TARGET_LAYOUT_QUERY_SIZE, &ilp32, XR_NATIVE_USIZE));
    ASSERT_EQ_INT(result.status, XR_TARGET_LAYOUT_QUERY_OK);
    ASSERT_EQ_INT(result.value, 4);
    result = XR_TARGET_LAYOUT_QUERY_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_TARGET_LAYOUT_QUERY_HI,
        XR_SEM_OWNER_ID_SHARED_TARGET_LAYOUT_QUERY_LO, XR_SEM_CONSUMER_CGEN,
        xr_target_layout_query_core(XR_TARGET_LAYOUT_QUERY_ALIGN, &lp64, XR_NATIVE_ISIZE));
    ASSERT_EQ_INT(result.status, XR_TARGET_LAYOUT_QUERY_OK);
    ASSERT_EQ_INT(result.value, 8);

    result = xr_target_layout_query_core((XrTargetLayoutQueryKind) 99, &lp64, XR_NATIVE_I64);
    ASSERT_EQ_INT(result.status, XR_TARGET_LAYOUT_QUERY_INVALID_KIND);
    result = xr_target_layout_query_core(XR_TARGET_LAYOUT_QUERY_SIZE, NULL, XR_NATIVE_I64);
    ASSERT_EQ_INT(result.status, XR_TARGET_LAYOUT_QUERY_INVALID_LAYOUT);
    XrTargetDataLayout corrupt = lp64;
    corrupt.stable_hash ^= UINT64_C(1);
    result = xr_target_layout_query_core(XR_TARGET_LAYOUT_QUERY_SIZE, &corrupt, XR_NATIVE_I64);
    ASSERT_EQ_INT(result.status, XR_TARGET_LAYOUT_QUERY_INVALID_LAYOUT);
    result = xr_target_layout_query_core(XR_TARGET_LAYOUT_QUERY_ALIGN, &lp64,
                                         XR_NATIVE_NESTED_AGGREGATE);
    ASSERT_EQ_INT(result.status, XR_TARGET_LAYOUT_QUERY_INVALID_NATIVE_TYPE);
}

TEST(target_aggregate_pointer_and_native_size_layouts) {
    XrTargetDataLayout ilp32;
    XrTargetDataLayout lp64;
    ASSERT_TRUE(xr_target_data_layout_init_ilp32(&ilp32));
    ASSERT_TRUE(xr_target_data_layout_init_lp64(&lp64));

    XrAggregateLayout pair32 = aggregate(XR_AGG_LAYOUT_STRUCT, 2);
    pair32.fields[0].native_type = XR_NATIVE_POINTER;
    pair32.fields[1].native_type = XR_NATIVE_USIZE;
    ASSERT_TRUE(xr_aggregate_layout_compute(&pair32, &ilp32));
    ASSERT_EQ_UINT(pair32.fields[0].offset, 0);
    ASSERT_EQ_UINT(pair32.fields[1].offset, 4);
    ASSERT_EQ_UINT(pair32.total_size, 8);
    ASSERT_EQ_UINT(pair32.alignment, 4);

    XrAggregateLayout pair64 = aggregate(XR_AGG_LAYOUT_STRUCT, 2);
    pair64.fields[0].native_type = XR_NATIVE_POINTER;
    pair64.fields[1].native_type = XR_NATIVE_USIZE;
    ASSERT_TRUE(xr_aggregate_layout_compute(&pair64, &lp64));
    ASSERT_EQ_UINT(pair64.fields[0].offset, 0);
    ASSERT_EQ_UINT(pair64.fields[1].offset, 8);
    ASSERT_EQ_UINT(pair64.total_size, 16);
    ASSERT_EQ_UINT(pair64.alignment, 8);

    XrAggregateLayout triple32 = aggregate(XR_AGG_LAYOUT_STRUCT, 3);
    triple32.fields[0].native_type = XR_NATIVE_U8;
    triple32.fields[1].native_type = XR_NATIVE_POINTER;
    triple32.fields[2].native_type = XR_NATIVE_ISIZE;
    ASSERT_TRUE(xr_aggregate_layout_compute(&triple32, &ilp32));
    ASSERT_EQ_UINT(triple32.fields[0].offset, 0);
    ASSERT_EQ_UINT(triple32.fields[1].offset, 4);
    ASSERT_EQ_UINT(triple32.fields[2].offset, 8);
    ASSERT_EQ_UINT(triple32.total_size, 12);

    XrAggregateLayout triple64 = aggregate(XR_AGG_LAYOUT_STRUCT, 3);
    triple64.fields[0].native_type = XR_NATIVE_U8;
    triple64.fields[1].native_type = XR_NATIVE_POINTER;
    triple64.fields[2].native_type = XR_NATIVE_ISIZE;
    ASSERT_TRUE(xr_aggregate_layout_compute(&triple64, &lp64));
    ASSERT_EQ_UINT(triple64.fields[0].offset, 0);
    ASSERT_EQ_UINT(triple64.fields[1].offset, 8);
    ASSERT_EQ_UINT(triple64.fields[2].offset, 16);
    ASSERT_EQ_UINT(triple64.total_size, 24);
}

TEST(target_aggregate_nested_packed_union_and_array_layouts) {
    XrTargetDataLayout ilp32;
    XrTargetDataLayout lp64;
    ASSERT_TRUE(xr_target_data_layout_init_ilp32(&ilp32));
    ASSERT_TRUE(xr_target_data_layout_init_lp64(&lp64));

    XrAggregateLayout child32 = aggregate(XR_AGG_LAYOUT_STRUCT, 2);
    child32.fields[0].native_type = XR_NATIVE_U8;
    child32.fields[1].native_type = XR_NATIVE_POINTER;
    ASSERT_TRUE(xr_aggregate_layout_compute(&child32, &ilp32));
    XrAggregateLayout outer32 = aggregate(XR_AGG_LAYOUT_STRUCT, 2);
    outer32.fields[0].native_type = XR_NATIVE_U16;
    outer32.fields[1].native_type = XR_NATIVE_NESTED_AGGREGATE;
    outer32.fields[1].sub_layout = &child32;
    ASSERT_TRUE(xr_aggregate_layout_compute(&outer32, &ilp32));
    ASSERT_EQ_UINT(outer32.fields[1].offset, 4);
    ASSERT_EQ_UINT(outer32.total_size, 12);

    XrAggregateLayout child64 = aggregate(XR_AGG_LAYOUT_STRUCT, 2);
    child64.fields[0].native_type = XR_NATIVE_U8;
    child64.fields[1].native_type = XR_NATIVE_POINTER;
    ASSERT_TRUE(xr_aggregate_layout_compute(&child64, &lp64));
    XrAggregateLayout outer64 = aggregate(XR_AGG_LAYOUT_STRUCT, 2);
    outer64.fields[0].native_type = XR_NATIVE_U16;
    outer64.fields[1].native_type = XR_NATIVE_NESTED_AGGREGATE;
    outer64.fields[1].sub_layout = &child64;
    ASSERT_TRUE(xr_aggregate_layout_compute(&outer64, &lp64));
    ASSERT_EQ_UINT(outer64.fields[1].offset, 8);
    ASSERT_EQ_UINT(outer64.total_size, 24);

    XrAggregateLayout packed32 = aggregate(XR_AGG_LAYOUT_PACKED_STRUCT, 2);
    packed32.fields[0].native_type = XR_NATIVE_U8;
    packed32.fields[1].native_type = XR_NATIVE_POINTER;
    ASSERT_TRUE(xr_aggregate_layout_compute(&packed32, &ilp32));
    ASSERT_EQ_UINT(packed32.fields[1].offset, 1);
    ASSERT_EQ_UINT(packed32.total_size, 5);
    ASSERT_EQ_UINT(packed32.alignment, 1);

    XrAggregateLayout packed64 = aggregate(XR_AGG_LAYOUT_PACKED_STRUCT, 2);
    packed64.fields[0].native_type = XR_NATIVE_U8;
    packed64.fields[1].native_type = XR_NATIVE_POINTER;
    ASSERT_TRUE(xr_aggregate_layout_compute(&packed64, &lp64));
    ASSERT_EQ_UINT(packed64.fields[1].offset, 1);
    ASSERT_EQ_UINT(packed64.total_size, 9);

    XrAggregateLayout union32 = aggregate(XR_AGG_LAYOUT_UNION, 2);
    union32.fields[0].native_type = XR_NATIVE_U8;
    union32.fields[1].native_type = XR_NATIVE_POINTER;
    ASSERT_TRUE(xr_aggregate_layout_compute(&union32, &ilp32));
    ASSERT_EQ_UINT(union32.total_size, 4);
    ASSERT_EQ_UINT(union32.alignment, 4);

    XrAggregateLayout union64 = aggregate(XR_AGG_LAYOUT_UNION, 2);
    union64.fields[0].native_type = XR_NATIVE_U8;
    union64.fields[1].native_type = XR_NATIVE_POINTER;
    ASSERT_TRUE(xr_aggregate_layout_compute(&union64, &lp64));
    ASSERT_EQ_UINT(union64.total_size, 8);
    ASSERT_EQ_UINT(union64.alignment, 8);

    XrAggregateLayout array32 = aggregate(XR_AGG_LAYOUT_STRUCT, 1);
    array32.fields[0].native_type = XR_NATIVE_ARRAY;
    array32.fields[0].elem_native_type = XR_NATIVE_POINTER;
    array32.fields[0].elem_count = 3;
    ASSERT_TRUE(xr_aggregate_layout_compute(&array32, &ilp32));
    ASSERT_EQ_UINT(array32.total_size, 12);

    XrAggregateLayout array64 = aggregate(XR_AGG_LAYOUT_STRUCT, 1);
    array64.fields[0].native_type = XR_NATIVE_ARRAY;
    array64.fields[0].elem_native_type = XR_NATIVE_POINTER;
    array64.fields[0].elem_count = 3;
    ASSERT_TRUE(xr_aggregate_layout_compute(&array64, &lp64));
    ASSERT_EQ_UINT(array64.total_size, 24);
}

TEST(target_aggregate_flexible_tail_and_endian_invariants) {
    XrTargetDataLayout little;
    XrTargetDataLayout big;
    ASSERT_TRUE(xr_target_data_layout_init(&little, 8, XR_TARGET_ENDIAN_LITTLE));
    ASSERT_TRUE(xr_target_data_layout_init(&big, 8, XR_TARGET_ENDIAN_BIG));

    XrAggregateLayout little_flex = aggregate(XR_AGG_LAYOUT_STRUCT, 2);
    little_flex.fields[0].native_type = XR_NATIVE_U8;
    little_flex.fields[1].native_type = XR_NATIVE_ARRAY;
    little_flex.fields[1].elem_native_type = XR_NATIVE_POINTER;
    little_flex.fields[1].is_flexible = true;
    ASSERT_TRUE(xr_aggregate_layout_compute(&little_flex, &little));
    ASSERT_EQ_UINT(little_flex.fields[1].offset, 8);
    ASSERT_EQ_UINT(little_flex.total_size, 8);
    ASSERT_EQ_UINT(little_flex.alignment, 8);

    XrAggregateLayout big_flex = aggregate(XR_AGG_LAYOUT_STRUCT, 2);
    big_flex.fields[0].native_type = XR_NATIVE_U8;
    big_flex.fields[1].native_type = XR_NATIVE_ARRAY;
    big_flex.fields[1].elem_native_type = XR_NATIVE_POINTER;
    big_flex.fields[1].is_flexible = true;
    ASSERT_TRUE(xr_aggregate_layout_compute(&big_flex, &big));
    ASSERT_EQ_UINT(big_flex.fields[1].offset, little_flex.fields[1].offset);
    ASSERT_EQ_UINT(big_flex.total_size, little_flex.total_size);
    ASSERT_EQ_UINT(big_flex.alignment, little_flex.alignment);
    ASSERT_NE(big.stable_hash, little.stable_hash);
    ASSERT_NE(big_flex.target_abi_hash, little_flex.target_abi_hash);
    ASSERT_NE(xr_aggregate_layout_stable_key(&big_flex),
              xr_aggregate_layout_stable_key(&little_flex));
}

TEST(native_type_alignment_rules_are_stable) {
    ASSERT_EQ_INT(xr_native_type_align(xr_target_data_layout_host(), XR_NATIVE_I8), 1);
    ASSERT_EQ_INT(xr_native_type_align(xr_target_data_layout_host(), XR_NATIVE_I16), 2);
    ASSERT_EQ_INT(xr_native_type_align(xr_target_data_layout_host(), XR_NATIVE_I32), 4);
    ASSERT_EQ_INT(xr_native_type_align(xr_target_data_layout_host(), XR_NATIVE_I64), 8);
    ASSERT_EQ_INT(xr_native_type_align(xr_target_data_layout_host(), XR_NATIVE_ARRAY), 0);
    ASSERT_EQ_INT(xr_native_type_align(xr_target_data_layout_host(), XR_NATIVE_ARRAY_REF), 8);
    ASSERT_EQ_INT(xr_native_type_align(xr_target_data_layout_host(), XR_NATIVE_MAP_REF), 8);
    ASSERT_EQ_INT(xr_native_type_align(xr_target_data_layout_host(), XR_NATIVE_SET_REF), 8);
    ASSERT_EQ_INT(xr_native_type_align(xr_target_data_layout_host(), XR_NATIVE_VALUE), 8);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Native Type Core");
RUN_TEST(native_type_tag_values_are_stable);
RUN_TEST(native_type_scalar_sizes_are_stable);
RUN_TEST(native_type_aggregate_sizes_are_stable);
RUN_TEST(native_type_alignment_rules_are_stable);
RUN_TEST(target_data_layout_distinguishes_ilp32_and_lp64);
RUN_TEST(target_layout_query_owner_covers_width_and_failure_edges);
RUN_TEST(target_aggregate_pointer_and_native_size_layouts);
RUN_TEST(target_aggregate_nested_packed_union_and_array_layouts);
RUN_TEST(target_aggregate_flexible_tail_and_endian_invariants);

TEST_MAIN_END()
