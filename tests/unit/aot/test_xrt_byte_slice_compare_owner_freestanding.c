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

_Noreturn void xr_hook_panic(const char *message, size_t len) {
    (void) fwrite(message, 1, len, stderr);
    (void) fputc('\n', stderr);
    abort();
}

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

TEST(freestanding_byte_slice_common_prefix_owner_preserves_word_boundaries) {
    uint8_t left_bytes[] = {1, 2, 3, 4, 5, 6, 7, 8, 128, 10, 11, 12, 13, 14, 15, 255, 17};
    uint8_t right_bytes[] = {1, 2, 3, 4, 5, 6, 7, 8, 128, 10, 11, 12, 13, 14, 15, 255, 17};
    xr_span_t left = {left_bytes, 17};
    xr_span_t right = {right_bytes, 17};
    xr_span_t short_right = {right_bytes, 9};
    xr_span_t empty = {NULL, 0};

    ASSERT_EQ_INT(xrt_byte_slice_common_prefix_checked_raw(left, right), 17);
    ASSERT_EQ_INT(xrt_byte_slice_common_prefix_checked_raw(left, short_right), 9);
    right_bytes[8] = 129;
    ASSERT_EQ_INT(xrt_byte_slice_common_prefix_checked_raw(left, right), 8);
    ASSERT_EQ_INT(xrt_byte_slice_common_prefix_checked_raw(empty, right), 0);
}

TEST(freestanding_byte_slice_fill_owner_preserves_boundaries) {
    uint8_t bytes[] = {1, 2, 3, 4};
    xr_span_t span = {bytes, 4};
    xr_span_t empty = {NULL, 0};

    xrt_byte_slice_fill_checked_raw(span, 511);
    ASSERT_EQ_INT(bytes[0], 255);
    ASSERT_EQ_INT(bytes[3], 255);
    xrt_byte_slice_fill_checked_raw(span, -2);
    ASSERT_EQ_INT(bytes[0], 254);
    ASSERT_EQ_INT(bytes[3], 254);
    ASSERT_EQ_PTR(xrt_byte_slice_fill_checked_raw(empty, 7).data, NULL);
    ASSERT_FALSE(xr_byte_slice_fill_core(NULL, 1, XR_ELEM_U8, 7));
    ASSERT_FALSE(xr_byte_slice_fill_core(bytes, -1, XR_ELEM_U8, 7));
    ASSERT_FALSE(xr_byte_slice_fill_core(bytes, 4, XR_ELEM_I64, 7));
}

TEST(freestanding_raw_memory_copy_owner_preserves_boundaries) {
    uint8_t source[40];
    uint8_t target[40];
    const int counts[] = {1, 2, 7, 8, 9, 16, 17, 31};
    for (int i = 0; i < 40; i++)
        source[i] = (uint8_t) (i + 19);
    ASSERT_EQ_PTR(xrt_raw_memory_copy_nonoverlap(NULL, NULL, 0), NULL);
    for (size_t n = 0; n < sizeof(counts) / sizeof(counts[0]); n++) {
        int count = counts[n];
        memset(target, 0xA5, sizeof(target));
        ASSERT_EQ_PTR(xrt_raw_memory_copy_nonoverlap(target + 2, source + 3, count), target + 2);
        ASSERT_EQ_INT(target[1], 0xA5);
        ASSERT_EQ_INT(target[2 + count], 0xA5);
        ASSERT_TRUE(memcmp(target + 2, source + 3, (size_t) count) == 0);
    }
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Freestanding Byte Slice Compare Owner");
RUN_TEST(freestanding_byte_slice_compare_owner_preserves_unsigned_and_prefix_ordering);
RUN_TEST(freestanding_byte_slice_common_prefix_owner_preserves_word_boundaries);
RUN_TEST(freestanding_byte_slice_fill_owner_preserves_boundaries);
RUN_TEST(freestanding_raw_memory_copy_owner_preserves_boundaries);
TEST_MAIN_END()
