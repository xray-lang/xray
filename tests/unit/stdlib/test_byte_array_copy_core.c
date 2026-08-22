/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_byte_array_copy_core.c - canonical Array<u8> copy owner KAT
 */

#include "../test_framework.h"
#include "shared/xr_byte_array_copy_core.h"

TEST(byte_array_copy_owner_preserves_both_overlap_directions) {
    uint8_t right[] = {1, 2, 3, 4, 5, 6};
    uint8_t left[] = {1, 2, 3, 4, 5, 6};
    XrByteArrayCopyResult result = XR_BYTE_ARRAY_COPY_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_HI,
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_LO, XR_SEM_CONSUMER_VM,
        xr_byte_array_copy_core(XR_BYTE_ARRAY_COPY_WITHIN, right, 6, XR_ELEM_U8, right, 6,
                                XR_ELEM_U8, 0, 2, 4));
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_OK);
    ASSERT(result.changed);
    ASSERT_EQ_INT(right[2], 1);
    ASSERT_EQ_INT(right[5], 4);

    result = xr_byte_array_copy_core(XR_BYTE_ARRAY_COPY_WITHIN, left, 6, XR_ELEM_U8, left, 6,
                                     XR_ELEM_U8, 2, 0, 4);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_OK);
    ASSERT_EQ_INT(left[0], 3);
    ASSERT_EQ_INT(left[3], 6);
}

TEST(byte_array_copy_owner_preserves_separate_and_aliased_views) {
    uint8_t src[] = {9, 8, 7, 6};
    uint8_t dst[] = {0, 0, 0, 0, 0, 0};
    uint8_t shared[] = {10, 20, 30, 40, 50, 60, 70};
    XrByteArrayCopyResult result = xr_byte_array_copy_core(
        XR_BYTE_ARRAY_COPY_FROM, dst, 6, XR_ELEM_U8, src, 4, XR_ELEM_U8, 1, 2, 3);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_OK);
    ASSERT_EQ_INT(dst[2], 8);
    ASSERT_EQ_INT(dst[4], 6);

    result = xr_byte_array_copy_core(XR_BYTE_ARRAY_COPY_FROM, shared + 1, 6, XR_ELEM_U8,
                                     shared, 6, XR_ELEM_U8, 0, 1, 5);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_OK);
    ASSERT_EQ_INT(shared[2], 10);
    ASSERT_EQ_INT(shared[6], 50);
}

TEST(byte_array_copy_owner_rejects_full_width_invalid_ranges_without_wrap) {
    uint8_t bytes[] = {1, 2, 3, 4};
    const int64_t wide = INT64_C(4294967296);
    XrByteArrayCopyResult result = xr_byte_array_copy_core(
        XR_BYTE_ARRAY_COPY_WITHIN, bytes, 4, XR_ELEM_U8, bytes, 4, XR_ELEM_U8, wide, 0, 1);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_OUT_OF_BOUNDS);
    result = xr_byte_array_copy_core(XR_BYTE_ARRAY_COPY_FROM, bytes, 4, XR_ELEM_U8, bytes, 4,
                                     XR_ELEM_U8, 0, wide, 1);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_OUT_OF_BOUNDS);
    result = xr_byte_array_copy_core(XR_BYTE_ARRAY_COPY_FROM, bytes, 4, XR_ELEM_U8, bytes, 4,
                                     XR_ELEM_U8, 0, 0, INT64_MAX);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_OUT_OF_BOUNDS);
    result = xr_byte_array_copy_core(XR_BYTE_ARRAY_COPY_WITHIN, bytes, 4, XR_ELEM_U8, bytes, 4,
                                     XR_ELEM_U8, -1, 0, 1);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_OUT_OF_BOUNDS);
}

TEST(byte_array_copy_owner_distinguishes_empty_invalid_type_and_missing_storage) {
    XrByteArrayCopyResult result = xr_byte_array_copy_core(
        XR_BYTE_ARRAY_COPY_FROM, NULL, 0, XR_ELEM_U8, NULL, 0, XR_ELEM_U8, 0, 0, 0);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_OK);
    ASSERT(!result.changed);

    result = xr_byte_array_copy_core(XR_BYTE_ARRAY_COPY_FROM, NULL, 1, XR_ELEM_U8, NULL, 1,
                                     XR_ELEM_U8, 0, 0, 1);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_NO_DATA);
    result = xr_byte_array_copy_core(XR_BYTE_ARRAY_COPY_FROM, NULL, 0, XR_ELEM_I64, NULL, 0,
                                     XR_ELEM_U8, 0, 0, 0);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_WRONG_ELEMENT_TYPE);
    result = xr_byte_array_copy_core((XrByteArrayCopyKind) 99, NULL, 0, XR_ELEM_U8, NULL, 0,
                                     XR_ELEM_U8, 0, 0, 0);
    ASSERT_EQ_INT(result.status, XR_BYTE_ARRAY_COPY_INVALID_KIND);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Byte Array Copy Core");
RUN_TEST(byte_array_copy_owner_preserves_both_overlap_directions);
RUN_TEST(byte_array_copy_owner_preserves_separate_and_aliased_views);
RUN_TEST(byte_array_copy_owner_rejects_full_width_invalid_ranges_without_wrap);
RUN_TEST(byte_array_copy_owner_distinguishes_empty_invalid_type_and_missing_storage);
TEST_MAIN_END()
