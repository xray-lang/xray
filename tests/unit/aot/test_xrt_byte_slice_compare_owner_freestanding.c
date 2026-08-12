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

TEST(freestanding_byte_slice_mutation_owners_preserve_overlap_and_boundaries) {
    uint8_t bytes[] = {1, 2, 3, 4, 5, 6, 0, 0, 0, 0};
    xr_span_t overlap_dst = {bytes + 1, 5};
    xr_span_t overlap_src = {bytes, 5};
    xr_span_t empty = {NULL, 0};

    xrt_byte_slice_copy_checked_raw(overlap_dst, overlap_src);
    ASSERT_EQ_INT(bytes[1], 1);
    ASSERT_EQ_INT(bytes[5], 5);
    ASSERT_EQ_PTR(xrt_byte_slice_copy_checked_raw(empty, empty).data, NULL);
    ASSERT_FALSE(xr_byte_slice_copy_core(NULL, 1, XR_ELEM_U8, bytes, 1, XR_ELEM_U8));
    ASSERT_FALSE(xr_byte_slice_copy_core(bytes, 1, XR_ELEM_U8, bytes, 2, XR_ELEM_U8));

    xrt_byte_slice_repeat_from_checked_raw((xr_span_t) {bytes, 10}, 6, 2, 4);
    ASSERT_EQ_INT(bytes[6], bytes[4]);
    ASSERT_EQ_INT(bytes[7], bytes[5]);
    ASSERT_EQ_INT(bytes[8], bytes[4]);
    ASSERT_EQ_INT(bytes[9], bytes[5]);
    ASSERT_FALSE(xr_byte_slice_repeat_core(NULL, 0, XR_ELEM_U8, 0, 1, 0));
    ASSERT_FALSE(xr_byte_slice_repeat_core(bytes, 10, XR_ELEM_U8, 0, 1, 1));
    ASSERT_FALSE(xr_byte_slice_repeat_core(bytes, 10, XR_ELEM_U8, 2, 0, 1));
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

TEST(freestanding_pod_slice_owners_preserve_overlap_and_byte_order) {
    uint32_t words[] = {UINT32_C(0x01020304), UINT32_C(0x11121314), UINT32_C(0x21222324),
                        UINT32_C(0x31323334)};
    uint8_t left_bytes[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t right_bytes[] = {1, 2, 3, 4, 5, 6, 7, 9};
    xr_span_t overlap_dst = {words + 1, 3};
    xr_span_t overlap_src = {words, 3};
    xr_span_t left = {left_bytes, 2};
    xr_span_t right = {right_bytes, 2};
    xr_span_t empty = {NULL, 0};

    ASSERT_EQ_PTR(xrt_span_copy_checked_raw(overlap_dst, overlap_src, sizeof(uint32_t)).data,
                  overlap_dst.data);
    ASSERT_EQ_INT(words[1], UINT32_C(0x01020304));
    ASSERT_EQ_INT(words[3], UINT32_C(0x21222324));
    ASSERT_EQ_INT(xrt_span_compare_checked_raw(left, right, sizeof(uint32_t)), -1);
    ASSERT_EQ_INT(xrt_span_compare_checked_raw(empty, empty, sizeof(uint32_t)), 0);
}

TEST(freestanding_pod_slice_view_owner_preserves_layout_edges) {
    uint32_t words[] = {UINT32_C(0x01020304), UINT32_C(0x11121314)};
    xr_span_t word_span = {words, 2};
    xr_span_t bytes = xrt_pod_slice_view_checked_raw(
        word_span, XR_POD_SLICE_VIEW_AS_BYTES, sizeof(uint32_t), true, 0, 0, 0, false, false);
    ASSERT_EQ_PTR(bytes.data, words);
    ASSERT_EQ_INT(bytes.length, 8);
    xr_span_t roundtrip = xrt_pod_slice_view_checked_raw(
        bytes, XR_POD_SLICE_VIEW_REINTERPRET, 1, true, sizeof(uint32_t), sizeof(uint32_t),
        _Alignof(uint32_t), true, false);
    ASSERT_EQ_PTR(roundtrip.data, words);
    ASSERT_EQ_INT(roundtrip.length, 2);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Freestanding Byte Slice Compare Owner");
RUN_TEST(freestanding_byte_slice_compare_owner_preserves_unsigned_and_prefix_ordering);
RUN_TEST(freestanding_byte_slice_common_prefix_owner_preserves_word_boundaries);
RUN_TEST(freestanding_byte_slice_fill_owner_preserves_boundaries);
RUN_TEST(freestanding_byte_slice_mutation_owners_preserve_overlap_and_boundaries);
RUN_TEST(freestanding_raw_memory_copy_owner_preserves_boundaries);
RUN_TEST(freestanding_pod_slice_owners_preserve_overlap_and_byte_order);
RUN_TEST(freestanding_pod_slice_view_owner_preserves_layout_edges);
TEST_MAIN_END()
