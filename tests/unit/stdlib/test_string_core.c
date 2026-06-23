/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_string_core.c - Unit tests for runtime-neutral string core helpers
 */

#include "../test_framework.h"
#include "../../../src/shared/xr_string_core.h"

static void assert_slice_eq(XrStringCoreSlice slice, const char *expected) {
    size_t expected_len = strlen(expected);
    ASSERT_EQ_UINT(slice.len, expected_len);
    ASSERT(memcmp(slice.data, expected, expected_len) == 0);
}

TEST(string_core_trim_both) {
    const char *s = " \t hello\r\n";
    assert_slice_eq(xr_string_core_trim_slice(s, strlen(s), XR_STRING_CORE_TRIM_BOTH), "hello");
}

TEST(string_core_trim_start) {
    const char *s = "\n\talpha  ";
    assert_slice_eq(xr_string_core_trim_slice(s, strlen(s), XR_STRING_CORE_TRIM_START), "alpha  ");
}

TEST(string_core_trim_end) {
    const char *s = "  beta\r\n";
    assert_slice_eq(xr_string_core_trim_slice(s, strlen(s), XR_STRING_CORE_TRIM_END), "  beta");
}

TEST(string_core_trim_all_whitespace) {
    const char *s = " \t\r\n";
    XrStringCoreSlice slice = xr_string_core_trim_slice(s, strlen(s), XR_STRING_CORE_TRIM_BOTH);
    ASSERT_EQ_UINT(slice.len, 0);
    ASSERT_EQ_PTR(slice.data, s + strlen(s));
}

TEST(string_core_trim_no_whitespace) {
    const char *s = "xray";
    XrStringCoreSlice slice = xr_string_core_trim_slice(s, strlen(s), XR_STRING_CORE_TRIM_BOTH);
    ASSERT_EQ_PTR(slice.data, s);
    ASSERT_EQ_UINT(slice.len, strlen(s));
}

TEST(string_core_trim_empty_and_null_zero) {
    const char *empty = "";
    XrStringCoreSlice empty_slice = xr_string_core_trim_slice(empty, 0, XR_STRING_CORE_TRIM_BOTH);
    ASSERT_EQ_PTR(empty_slice.data, empty);
    ASSERT_EQ_UINT(empty_slice.len, 0);

    XrStringCoreSlice null_zero = xr_string_core_trim_slice(NULL, 0, XR_STRING_CORE_TRIM_BOTH);
    ASSERT_EQ_PTR(null_zero.data, NULL);
    ASSERT_EQ_UINT(null_zero.len, 0);
}

TEST(string_core_index_of_basic_and_long_pattern) {
    const char *s = "hello world";
    ASSERT_EQ_INT(xr_string_core_index_of(s, strlen(s), "hello", 5), 0);
    ASSERT_EQ_INT(xr_string_core_index_of(s, strlen(s), "world", 5), 6);
    ASSERT_EQ_INT(xr_string_core_index_of(s, strlen(s), "o", 1), 4);
    ASSERT_EQ_INT(xr_string_core_index_of(s, strlen(s), "", 0), 0);
    ASSERT_EQ_INT(xr_string_core_index_of(s, strlen(s), "missing", 7), -1);

    const char *long_s = "prefix--longneedle--suffix--longneedle";
    ASSERT_EQ_INT(xr_string_core_index_of(long_s, strlen(long_s), "longneedle", 10), 8);
    ASSERT_EQ_INT(xr_string_core_last_index_of(long_s, strlen(long_s), "longneedle", 10), 28);
}

TEST(string_core_index_of_embedded_nul) {
    const char s[] = {'a', 'b', '\0', 'c', 'd', '\0', 'e'};
    const char needle1[] = {'\0', 'c'};
    const char needle2[] = {'d', '\0', 'e'};
    const char missing[] = {'c', '\0', 'x'};

    ASSERT_EQ_INT(xr_string_core_index_of(s, sizeof(s), needle1, sizeof(needle1)), 2);
    ASSERT_EQ_INT(xr_string_core_index_of(s, sizeof(s), needle2, sizeof(needle2)), 4);
    ASSERT_EQ_INT(xr_string_core_index_of(s, sizeof(s), missing, sizeof(missing)), -1);
    ASSERT_TRUE(xr_string_core_contains(s, sizeof(s), needle2, sizeof(needle2)));
}

TEST(string_core_last_index_of_edges) {
    const char *s = "aaaaa";
    ASSERT_EQ_INT(xr_string_core_last_index_of(s, strlen(s), "aa", 2), 3);
    ASSERT_EQ_INT(xr_string_core_last_index_of(s, strlen(s), "", 0), 5);
    ASSERT_EQ_INT(xr_string_core_last_index_of(s, strlen(s), "aaaaaa", 6), -1);
    ASSERT_EQ_INT(xr_string_core_last_index_of(NULL, 0, "", 0), 0);
}

TEST(string_core_prefix_suffix) {
    const char s[] = {'a', 'b', '\0', 'c', 'd', '\0', 'e'};
    const char prefix[] = {'a', 'b', '\0'};
    const char suffix[] = {'d', '\0', 'e'};

    ASSERT_TRUE(xr_string_core_starts_with(s, sizeof(s), prefix, sizeof(prefix)));
    ASSERT_TRUE(xr_string_core_ends_with(s, sizeof(s), suffix, sizeof(suffix)));
    ASSERT_TRUE(xr_string_core_starts_with(s, sizeof(s), "", 0));
    ASSERT_TRUE(xr_string_core_ends_with(s, sizeof(s), "", 0));
    ASSERT_FALSE(xr_string_core_starts_with(s, sizeof(s), "abx", 3));
    ASSERT_FALSE(xr_string_core_ends_with(s, sizeof(s), "xee", 3));
    ASSERT_FALSE(xr_string_core_starts_with(NULL, 1, "a", 1));
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("String Core");
RUN_TEST(string_core_trim_both);
RUN_TEST(string_core_trim_start);
RUN_TEST(string_core_trim_end);
RUN_TEST(string_core_trim_all_whitespace);
RUN_TEST(string_core_trim_no_whitespace);
RUN_TEST(string_core_trim_empty_and_null_zero);
RUN_TEST(string_core_index_of_basic_and_long_pattern);
RUN_TEST(string_core_index_of_embedded_nul);
RUN_TEST(string_core_last_index_of_edges);
RUN_TEST(string_core_prefix_suffix);

TEST_MAIN_END()
