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
#include "base/xtarget_data_layout.h"
#include "shared/xr_native_type_core.h"

#include <string.h>

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

TEST_MAIN_END()
