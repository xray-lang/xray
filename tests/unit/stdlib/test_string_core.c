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

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("String Core");
RUN_TEST(string_core_trim_both);
RUN_TEST(string_core_trim_start);
RUN_TEST(string_core_trim_end);
RUN_TEST(string_core_trim_all_whitespace);
RUN_TEST(string_core_trim_no_whitespace);
RUN_TEST(string_core_trim_empty_and_null_zero);

TEST_MAIN_END()
