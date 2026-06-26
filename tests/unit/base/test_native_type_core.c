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
#include "shared/xr_native_type_core.h"

TEST(native_type_tag_values_are_stable) {
    ASSERT_EQ_INT(XR_NATIVE_I64, 0);
    ASSERT_EQ_INT(XR_NATIVE_F64, 1);
    ASSERT_EQ_INT(XR_NATIVE_BOOL, 2);
    ASSERT_EQ_INT(XR_NATIVE_ARRAY_REF, 14);
    ASSERT_EQ_INT(XR_NATIVE_MAP_REF, 15);
    ASSERT_EQ_INT(XR_NATIVE_SET_REF, 16);
}

TEST(native_type_scalar_sizes_are_stable) {
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_I8), 1);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_U8), 1);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_BOOL), 1);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_I16), 2);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_U16), 2);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_I32), 4);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_U32), 4);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_F32), 4);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_I64), 8);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_U64), 8);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_F64), 8);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_STRING), 8);
}

TEST(native_type_aggregate_sizes_are_stable) {
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_STRUCT), 8);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_ARRAY), 0);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_ARRAY_REF), 16);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_MAP_REF), 16);
    ASSERT_EQ_INT(xr_native_type_size(XR_NATIVE_SET_REF), 16);
}

TEST(native_type_alignment_rules_are_stable) {
    ASSERT_EQ_INT(xr_native_type_align(XR_NATIVE_I8), 1);
    ASSERT_EQ_INT(xr_native_type_align(XR_NATIVE_I16), 2);
    ASSERT_EQ_INT(xr_native_type_align(XR_NATIVE_I32), 4);
    ASSERT_EQ_INT(xr_native_type_align(XR_NATIVE_I64), 8);
    ASSERT_EQ_INT(xr_native_type_align(XR_NATIVE_ARRAY), 0);
    ASSERT_EQ_INT(xr_native_type_align(XR_NATIVE_ARRAY_REF), 8);
    ASSERT_EQ_INT(xr_native_type_align(XR_NATIVE_MAP_REF), 8);
    ASSERT_EQ_INT(xr_native_type_align(XR_NATIVE_SET_REF), 8);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Native Type Core");
RUN_TEST(native_type_tag_values_are_stable);
RUN_TEST(native_type_scalar_sizes_are_stable);
RUN_TEST(native_type_aggregate_sizes_are_stable);
RUN_TEST(native_type_alignment_rules_are_stable);

TEST_MAIN_END()
