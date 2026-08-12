/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xrt_byte_array_copy_owner_freestanding.c - freestanding copy adapter KAT
 */

#include "../test_framework.h"
#include "aot/xrt_core_freestanding.h"

TEST(freestanding_byte_array_copy_adapter_preserves_overlap) {
    uint8_t bytes[] = {1, 2, 3, 4, 5, 6};
    XrByteArrayCopyResult result = xrt_byte_array_copy_semantics(
        XR_BYTE_ARRAY_COPY_WITHIN, bytes, 6, XR_ELEM_U8, bytes, 6, XR_ELEM_U8, 0, 2, 4);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_OK);
    ASSERT(result.changed);
    ASSERT_EQ_INT(bytes[2], 1);
    ASSERT_EQ_INT(bytes[5], 4);
}

TEST(freestanding_byte_array_copy_adapter_preserves_full_width_ranges) {
    uint8_t bytes[] = {1, 2, 3, 4};
    XrByteArrayCopyResult result = xrt_byte_array_copy_semantics(
        XR_BYTE_ARRAY_COPY_FROM, bytes, 4, XR_ELEM_U8, bytes, 4, XR_ELEM_U8,
        INT64_C(4294967296), 0, 1);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_OUT_OF_BOUNDS);
    result = xrt_byte_array_copy_semantics(XR_BYTE_ARRAY_COPY_FROM, NULL, 0, XR_ELEM_U8, NULL, 0,
                                           XR_ELEM_U8, 0, 0, 0);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_OK);
    ASSERT(!result.changed);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Freestanding Byte Array Copy Owner");
RUN_TEST(freestanding_byte_array_copy_adapter_preserves_overlap);
RUN_TEST(freestanding_byte_array_copy_adapter_preserves_full_width_ranges);
TEST_MAIN_END()
