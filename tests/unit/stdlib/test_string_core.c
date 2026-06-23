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
    if (expected_len != 0)
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

TEST(string_core_ascii_case_write) {
    char lower[32];
    ASSERT_EQ_UINT(xr_string_core_ascii_lower_write(lower, "Hello XRay 123!", 15), 15);
    ASSERT(strcmp(lower, "hello xray 123!") == 0);

    char upper[32];
    ASSERT_EQ_UINT(xr_string_core_ascii_upper_write(upper, "Hello XRay 123!", 15), 15);
    ASSERT(strcmp(upper, "HELLO XRAY 123!") == 0);
}

TEST(string_core_ascii_case_preserves_utf8) {
    const char *mixed = "Äx你Y🌍";
    char lower[32];
    ASSERT_EQ_UINT(xr_string_core_ascii_lower_write(lower, mixed, strlen(mixed)), strlen(mixed));
    ASSERT(strcmp(lower, "Äx你y🌍") == 0);

    char upper[32];
    ASSERT_EQ_UINT(xr_string_core_ascii_upper_write(upper, mixed, strlen(mixed)), strlen(mixed));
    ASSERT(strcmp(upper, "ÄX你Y🌍") == 0);
}

TEST(string_core_ascii_case_empty_and_null_zero) {
    char out[4] = {'x', 'x', 'x', '\0'};
    ASSERT_EQ_UINT(xr_string_core_ascii_lower_write(out, "", 0), 0);
    ASSERT(strcmp(out, "") == 0);

    out[0] = 'x';
    ASSERT_EQ_UINT(xr_string_core_ascii_upper_write(out, NULL, 0), 0);
    ASSERT(strcmp(out, "") == 0);

    ASSERT_EQ_UINT(xr_string_core_ascii_lower_write(NULL, "abc", 3), 0);
    ASSERT_EQ_UINT(xr_string_core_ascii_upper_write(NULL, "abc", 3), 0);
}

TEST(string_core_reverse_utf8) {
    char ascii[16];
    ASSERT_EQ_UINT(xr_string_core_reverse_utf8_write(ascii, "Hello", 5), 5);
    ASSERT(strcmp(ascii, "olleH") == 0);

    const char *mixed = "你a好🌍";
    const char *expected = "🌍好a你";
    char utf8[32];
    size_t len = strlen(mixed);
    ASSERT_EQ_UINT(xr_string_core_reverse_utf8_write(utf8, mixed, len), len);
    ASSERT(strcmp(utf8, expected) == 0);
}

TEST(string_core_reverse_empty_and_null_zero) {
    char out[4] = {'x', 'x', 'x', '\0'};
    ASSERT_EQ_UINT(xr_string_core_reverse_utf8_write(out, "", 0), 0);
    ASSERT(strcmp(out, "") == 0);

    out[0] = 'x';
    ASSERT_EQ_UINT(xr_string_core_reverse_utf8_write(out, NULL, 0), 0);
    ASSERT(strcmp(out, "") == 0);

    ASSERT_EQ_UINT(xr_string_core_reverse_utf8_write(NULL, "abc", 3), 0);
}

TEST(string_core_repeat_plan_and_write) {
    XrStringCoreRepeatPlan plan = xr_string_core_repeat_plan("ab", 2, 3);
    ASSERT_EQ_INT(plan.kind, XR_STRING_CORE_REPEAT_ALLOC);
    ASSERT_EQ_UINT(plan.len, 6);

    char out[32];
    ASSERT_EQ_UINT(xr_string_core_repeat_write(out, "ab", 2, 3), 6);
    ASSERT(strcmp(out, "ababab") == 0);

    ASSERT_EQ_UINT(xr_string_core_repeat_write(out, "好", strlen("好"), 2), strlen("好好"));
    ASSERT(strcmp(out, "好好") == 0);

    plan = xr_string_core_repeat_plan("abc", 3, 1);
    ASSERT_EQ_INT(plan.kind, XR_STRING_CORE_REPEAT_ORIGINAL);
    ASSERT_EQ_UINT(plan.len, 3);

    plan = xr_string_core_repeat_plan("abc", 3, 0);
    ASSERT_EQ_INT(plan.kind, XR_STRING_CORE_REPEAT_EMPTY);
    ASSERT_EQ_UINT(xr_string_core_repeat_write(out, "abc", 3, -4), 0);
    ASSERT(strcmp(out, "") == 0);

    plan = xr_string_core_repeat_plan("x", (SIZE_MAX / 2u) + 1u, 3);
    ASSERT_EQ_INT(plan.kind, XR_STRING_CORE_REPEAT_INVALID);
    ASSERT_EQ_INT(xr_string_core_repeat_plan(NULL, 1, 2).kind, XR_STRING_CORE_REPEAT_INVALID);
}

TEST(string_core_pad_plan_and_write) {
    XrStringCorePadPlan plan = xr_string_core_pad_plan("42", 2, 5, "0", 1);
    ASSERT_EQ_INT(plan.kind, XR_STRING_CORE_PAD_ALLOC);
    ASSERT_EQ_UINT(plan.len, 5);
    ASSERT_EQ_UINT(plan.fill_len, 3);

    char out[32];
    ASSERT_EQ_UINT(xr_string_core_pad_write(out, "42", 2, plan, XR_STRING_CORE_PAD_START), 5);
    ASSERT(strcmp(out, "00042") == 0);

    plan = xr_string_core_pad_plan("hi", 2, 5, "!", 1);
    ASSERT_EQ_UINT(xr_string_core_pad_write(out, "hi", 2, plan, XR_STRING_CORE_PAD_END), 5);
    ASSERT(strcmp(out, "hi!!!") == 0);

    plan = xr_string_core_pad_plan("ab", 2, 7, "01", 2);
    ASSERT_EQ_UINT(xr_string_core_pad_write(out, "ab", 2, plan, XR_STRING_CORE_PAD_START), 7);
    ASSERT(strcmp(out, "01010ab") == 0);

    plan = xr_string_core_pad_plan("x", 1, 3, NULL, 0);
    ASSERT_EQ_UINT(xr_string_core_pad_write(out, "x", 1, plan, XR_STRING_CORE_PAD_START), 3);
    ASSERT(strcmp(out, "  x") == 0);

    plan = xr_string_core_pad_plan("abc", 3, 5, "", 0);
    ASSERT_EQ_INT(plan.kind, XR_STRING_CORE_PAD_ORIGINAL);
    ASSERT_EQ_UINT(plan.len, 3);

    plan = xr_string_core_pad_plan("abc", 3, -1, "0", 1);
    ASSERT_EQ_INT(plan.kind, XR_STRING_CORE_PAD_ORIGINAL);

    ASSERT_EQ_INT(xr_string_core_pad_plan(NULL, 1, 2, "0", 1).kind, XR_STRING_CORE_PAD_INVALID);
    ASSERT_EQ_INT(xr_string_core_pad_plan("x", 1, 3, NULL, 1).kind, XR_STRING_CORE_PAD_INVALID);
}

TEST(string_core_replace_plan_and_write) {
    char out[32];
    XrStringCoreReplacePlan plan =
        xr_string_core_replace_plan("hello world", 11, "world", 5, "xray", 4, false);
    ASSERT_EQ_INT(plan.kind, XR_STRING_CORE_REPLACE_ALLOC);
    ASSERT_EQ_UINT(plan.len, 10);
    ASSERT_EQ_UINT(plan.count, 1);
    ASSERT_EQ_UINT(
        xr_string_core_replace_write(out, "hello world", 11, "world", 5, "xray", 4, plan, false),
        10);
    ASSERT(strcmp(out, "hello xray") == 0);

    plan = xr_string_core_replace_plan("aaaa", 4, "aa", 2, "X", 1, false);
    ASSERT_EQ_UINT(xr_string_core_replace_write(out, "aaaa", 4, "aa", 2, "X", 1, plan, false), 3);
    ASSERT(strcmp(out, "Xaa") == 0);

    plan = xr_string_core_replace_plan("aaaa", 4, "aa", 2, "X", 1, true);
    ASSERT_EQ_UINT(plan.count, 2);
    ASSERT_EQ_UINT(xr_string_core_replace_write(out, "aaaa", 4, "aa", 2, "X", 1, plan, true), 2);
    ASSERT(strcmp(out, "XX") == 0);

    plan = xr_string_core_replace_plan("abc", 3, "b", 1, "", 0, true);
    ASSERT_EQ_UINT(xr_string_core_replace_write(out, "abc", 3, "b", 1, "", 0, plan, true), 2);
    ASSERT(strcmp(out, "ac") == 0);

    plan = xr_string_core_replace_plan("abc", 3, "", 0, "x", 1, true);
    ASSERT_EQ_INT(plan.kind, XR_STRING_CORE_REPLACE_ORIGINAL);
    plan = xr_string_core_replace_plan("abc", 3, "z", 1, "x", 1, true);
    ASSERT_EQ_INT(plan.kind, XR_STRING_CORE_REPLACE_ORIGINAL);

    const char embedded[] = {'a', '\0', 'b', '\0', 'b'};
    const char needle[] = {'\0', 'b'};
    plan = xr_string_core_replace_plan(embedded, sizeof(embedded), needle, sizeof(needle), "X", 1,
                                       true);
    ASSERT_EQ_INT(plan.kind, XR_STRING_CORE_REPLACE_ALLOC);
    ASSERT_EQ_UINT(plan.len, 3);
    ASSERT_EQ_UINT(plan.count, 2);
    ASSERT_EQ_UINT(xr_string_core_replace_write(out, embedded, sizeof(embedded), needle,
                                                sizeof(needle), "X", 1, plan, true),
                   3);
    ASSERT(memcmp(out, "aXX", 3) == 0);

    ASSERT_EQ_INT(xr_string_core_replace_plan(NULL, 1, "a", 1, "b", 1, false).kind,
                  XR_STRING_CORE_REPLACE_INVALID);
    ASSERT_EQ_INT(xr_string_core_replace_plan("a", 1, NULL, 1, "b", 1, false).kind,
                  XR_STRING_CORE_REPLACE_INVALID);
    ASSERT_EQ_INT(xr_string_core_replace_plan("a", 1, "a", 1, NULL, 1, false).kind,
                  XR_STRING_CORE_REPLACE_INVALID);
}

TEST(string_core_parse_int64) {
    XrStringCoreParseIntResult parsed =
        xr_string_core_parse_int64(" \t\r\n-123tail", strlen(" \t\r\n-123tail"));
    ASSERT_TRUE(parsed.ok);
    ASSERT_EQ_INT(parsed.value, -123);

    parsed = xr_string_core_parse_int64("+42", 3);
    ASSERT_TRUE(parsed.ok);
    ASSERT_EQ_INT(parsed.value, 42);

    const char bounded[] = {'1', '2', '3', '4'};
    parsed = xr_string_core_parse_int64(bounded, 2);
    ASSERT_TRUE(parsed.ok);
    ASSERT_EQ_INT(parsed.value, 12);

    parsed = xr_string_core_parse_int64("abc", 3);
    ASSERT_FALSE(parsed.ok);

    parsed = xr_string_core_parse_int64("   ", 3);
    ASSERT_FALSE(parsed.ok);

    ASSERT_FALSE(xr_string_core_parse_int64(NULL, 0).ok);
}

TEST(string_core_parse_float64) {
    XrStringCoreParseFloatResult parsed =
        xr_string_core_parse_float64(" \n-3.5e2tail", strlen(" \n-3.5e2tail"));
    ASSERT_TRUE(parsed.ok);
    ASSERT_FLOAT_EQ(parsed.value, -350.0, 0.000001);

    parsed = xr_string_core_parse_float64("+0.25", 5);
    ASSERT_TRUE(parsed.ok);
    ASSERT_FLOAT_EQ(parsed.value, 0.25, 0.000001);

    const char bounded[] = {'1', '.', '2', '5', '9'};
    parsed = xr_string_core_parse_float64(bounded, 4);
    ASSERT_TRUE(parsed.ok);
    ASSERT_FLOAT_EQ(parsed.value, 1.25, 0.000001);

    parsed = xr_string_core_parse_float64("nope", 4);
    ASSERT_FALSE(parsed.ok);

    parsed = xr_string_core_parse_float64("\t", 1);
    ASSERT_FALSE(parsed.ok);

    ASSERT_FALSE(xr_string_core_parse_float64(NULL, 0).ok);
}

TEST(string_core_substring_bounds) {
    const char *s = "abcdef";
    assert_slice_eq(xr_string_core_substring_slice(s, strlen(s), -3, 3), "abc");
    assert_slice_eq(xr_string_core_substring_slice(s, strlen(s), 2, -1), "cdef");
    assert_slice_eq(xr_string_core_substring_slice(s, strlen(s), 0, 99), "abcdef");
    assert_slice_eq(xr_string_core_substring_slice(s, strlen(s), 9, 12), "");
    assert_slice_eq(xr_string_core_substring_slice(s, strlen(s), 4, 2), "");
}

TEST(string_core_slice_bounds) {
    const char *s = "abcdef";
    assert_slice_eq(xr_string_core_range_slice(s, strlen(s), -3, -1), "de");
    assert_slice_eq(xr_string_core_range_slice(s, strlen(s), 2, -1), "cde");
    assert_slice_eq(xr_string_core_range_slice(s, strlen(s), -100, 99), "abcdef");
    assert_slice_eq(xr_string_core_range_slice(s, strlen(s), 9, 12), "");
    assert_slice_eq(xr_string_core_range_slice(s, strlen(s), 4, 2), "");
}

TEST(string_core_utf8_char_slice) {
    const char *s = "A你🌍";
    ASSERT_EQ_UINT(xr_string_core_utf8_char_count(s, strlen(s)), 3);
    assert_slice_eq(xr_string_core_utf8_char_slice_at(s, strlen(s), 0), "A");
    assert_slice_eq(xr_string_core_utf8_char_slice_at(s, strlen(s), 1), "你");
    assert_slice_eq(xr_string_core_utf8_char_slice_at(s, strlen(s), 2), "🌍");
    assert_slice_eq(xr_string_core_utf8_char_slice_at(s, strlen(s), -1), "🌍");
    assert_slice_eq(xr_string_core_utf8_char_slice_at(s, strlen(s), 99), "");
    assert_slice_eq(xr_string_core_utf8_char_slice_at(s, strlen(s), -99), "");
}

TEST(string_core_byte_slice_and_codepoint) {
    const char *bytes = "é€";
    assert_slice_eq(xr_string_core_byte_slice_at(bytes, strlen(bytes), 0), "\xC3");
    assert_slice_eq(xr_string_core_byte_slice_at(bytes, strlen(bytes), 1), "\xA9");
    assert_slice_eq(xr_string_core_byte_slice_at(bytes, strlen(bytes), -1), "\xAC");
    assert_slice_eq(xr_string_core_byte_slice_at(bytes, strlen(bytes), 99), "");

    uint32_t cp = 0;
    ASSERT_TRUE(xr_string_core_codepoint_at("A", 1, 0, &cp));
    ASSERT_EQ_UINT(cp, 65);
    ASSERT_TRUE(xr_string_core_codepoint_at("世界", strlen("世界"), 1, &cp));
    ASSERT_EQ_UINT(cp, 30028);
    ASSERT_FALSE(xr_string_core_codepoint_at("A", 1, -1, &cp));
    ASSERT_FALSE(xr_string_core_codepoint_at("A", 1, 9, &cp));
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
RUN_TEST(string_core_ascii_case_write);
RUN_TEST(string_core_ascii_case_preserves_utf8);
RUN_TEST(string_core_ascii_case_empty_and_null_zero);
RUN_TEST(string_core_reverse_utf8);
RUN_TEST(string_core_reverse_empty_and_null_zero);
RUN_TEST(string_core_repeat_plan_and_write);
RUN_TEST(string_core_pad_plan_and_write);
RUN_TEST(string_core_replace_plan_and_write);
RUN_TEST(string_core_parse_int64);
RUN_TEST(string_core_parse_float64);
RUN_TEST(string_core_substring_bounds);
RUN_TEST(string_core_slice_bounds);
RUN_TEST(string_core_utf8_char_slice);
RUN_TEST(string_core_byte_slice_and_codepoint);

TEST_MAIN_END()
