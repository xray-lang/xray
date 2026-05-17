/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xstring.c - Unit tests for String object
 */

#include "../test_framework.h"
#include "../test_helper.h"
#include "runtime/object/xstring.h"

static XrayIsolate *X = NULL;

/* ========== Setup / Teardown ========== */

static void setup(void) {
    X = xray_isolate_new(NULL);
    ASSERT_NOT_NULL(X);
    // Initialize test coroutine (needed for some string operations)
    xr_test_init_coro(X);
}

static void teardown(void) {
    if (X) {
        xray_isolate_delete(X);
        X = NULL;
    }
}

/* ========== String Creation Tests ========== */

TEST(string_intern_empty) {
    setup();
    XrString *s = xr_string_intern(X, "", 0, xr_string_hash("", 0));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_UINT(s->length, 0);
    ASSERT_STR_EQ(s->data, "");
    teardown();
}

TEST(string_intern_simple) {
    setup();
    const char *text = "hello";
    XrString *s = xr_string_intern(X, text, 5, xr_string_hash(text, 5));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_UINT(s->length, 5);
    ASSERT_STR_EQ(s->data, "hello");
    teardown();
}

TEST(string_intern_dedup) {
    setup();
    const char *text = "test";
    uint32_t hash = xr_string_hash(text, 4);
    XrString *s1 = xr_string_intern(X, text, 4, hash);
    XrString *s2 = xr_string_intern(X, text, 4, hash);
    // Should return the same pointer (interned)
    ASSERT_EQ_PTR(s1, s2);
    teardown();
}

TEST(string_intern_different) {
    setup();
    XrString *s1 = xr_string_intern(X, "abc", 3, xr_string_hash("abc", 3));
    XrString *s2 = xr_string_intern(X, "def", 3, xr_string_hash("def", 3));
    ASSERT_NE(s1, s2);
    ASSERT_STR_EQ(s1->data, "abc");
    ASSERT_STR_EQ(s2->data, "def");
    teardown();
}

TEST(string_from_int) {
    setup();
    XrString *s = xr_string_from_int(X, 12345);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s->data, "12345");
    teardown();
}

TEST(string_from_int_negative) {
    setup();
    XrString *s = xr_string_from_int(X, -9876);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s->data, "-9876");
    teardown();
}

TEST(string_from_int_zero) {
    setup();
    XrString *s = xr_string_from_int(X, 0);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s->data, "0");
    teardown();
}

TEST(string_from_float) {
    setup();
    XrString *s = xr_string_from_float(X, 3.14);
    ASSERT_NOT_NULL(s);
    // Float formatting may vary, just check it contains "3.14"
    ASSERT_NOT_NULL(strstr(s->data, "3.14"));
    teardown();
}

/* ========== String Comparison Tests ========== */

TEST(string_equal_same) {
    setup();
    XrString *s1 = xr_string_intern(X, "test", 4, xr_string_hash("test", 4));
    XrString *s2 = xr_string_intern(X, "test", 4, xr_string_hash("test", 4));
    ASSERT_TRUE(xr_string_equal(s1, s2));
    teardown();
}

TEST(string_equal_different) {
    setup();
    XrString *s1 = xr_string_intern(X, "abc", 3, xr_string_hash("abc", 3));
    XrString *s2 = xr_string_intern(X, "def", 3, xr_string_hash("def", 3));
    ASSERT_FALSE(xr_string_equal(s1, s2));
    teardown();
}

TEST(string_equal_different_length) {
    setup();
    XrString *s1 = xr_string_intern(X, "ab", 2, xr_string_hash("ab", 2));
    XrString *s2 = xr_string_intern(X, "abc", 3, xr_string_hash("abc", 3));
    ASSERT_FALSE(xr_string_equal(s1, s2));
    teardown();
}

TEST(string_compare_equal) {
    setup();
    XrString *s1 = xr_string_intern(X, "abc", 3, xr_string_hash("abc", 3));
    XrString *s2 = xr_string_intern(X, "abc", 3, xr_string_hash("abc", 3));
    ASSERT_EQ_INT(xr_string_compare(s1, s2), 0);
    teardown();
}

TEST(string_compare_less) {
    setup();
    XrString *s1 = xr_string_intern(X, "abc", 3, xr_string_hash("abc", 3));
    XrString *s2 = xr_string_intern(X, "abd", 3, xr_string_hash("abd", 3));
    ASSERT_LT(xr_string_compare(s1, s2), 0);
    teardown();
}

TEST(string_compare_greater) {
    setup();
    XrString *s1 = xr_string_intern(X, "abd", 3, xr_string_hash("abd", 3));
    XrString *s2 = xr_string_intern(X, "abc", 3, xr_string_hash("abc", 3));
    ASSERT_GT(xr_string_compare(s1, s2), 0);
    teardown();
}

/* ========== String Concatenation Tests ========== */

TEST(string_concat_basic) {
    setup();
    XrString *a = xr_string_intern(X, "hello", 5, xr_string_hash("hello", 5));
    XrString *b = xr_string_intern(X, "world", 5, xr_string_hash("world", 5));
    XrString *c = xr_string_concat(X, a, b);
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(c->data, "helloworld");
    ASSERT_EQ_UINT(c->length, 10);
    teardown();
}

TEST(string_concat_empty_left) {
    setup();
    XrString *a = xr_string_intern(X, "", 0, xr_string_hash("", 0));
    XrString *b = xr_string_intern(X, "test", 4, xr_string_hash("test", 4));
    XrString *c = xr_string_concat(X, a, b);
    ASSERT_STR_EQ(c->data, "test");
    teardown();
}

TEST(string_concat_empty_right) {
    setup();
    XrString *a = xr_string_intern(X, "test", 4, xr_string_hash("test", 4));
    XrString *b = xr_string_intern(X, "", 0, xr_string_hash("", 0));
    XrString *c = xr_string_concat(X, a, b);
    ASSERT_STR_EQ(c->data, "test");
    teardown();
}

/* ========== String Method Tests ========== */

TEST(string_size) {
    setup();
    XrString *s = xr_string_intern(X, "hello", 5, xr_string_hash("hello", 5));
    ASSERT_EQ_INT(xr_string_size(X, s), 5);
    teardown();
}

TEST(string_is_empty) {
    setup();
    XrString *empty = xr_string_intern(X, "", 0, xr_string_hash("", 0));
    XrString *nonempty = xr_string_intern(X, "x", 1, xr_string_hash("x", 1));
    ASSERT_TRUE(xr_string_is_empty(X, empty));
    ASSERT_FALSE(xr_string_is_empty(X, nonempty));
    teardown();
}

TEST(string_index_of_found) {
    setup();
    XrString *s = xr_string_intern(X, "hello world", 11, xr_string_hash("hello world", 11));
    XrString *sub = xr_string_intern(X, "world", 5, xr_string_hash("world", 5));
    ASSERT_EQ_INT(xr_string_index_of(X, s, sub), 6);
    teardown();
}

TEST(string_index_of_not_found) {
    setup();
    XrString *s = xr_string_intern(X, "hello", 5, xr_string_hash("hello", 5));
    XrString *sub = xr_string_intern(X, "xyz", 3, xr_string_hash("xyz", 3));
    ASSERT_EQ_INT(xr_string_index_of(X, s, sub), -1);
    teardown();
}

TEST(string_has) {
    setup();
    XrString *s = xr_string_intern(X, "hello world", 11, xr_string_hash("hello world", 11));
    XrString *sub1 = xr_string_intern(X, "world", 5, xr_string_hash("world", 5));
    XrString *sub2 = xr_string_intern(X, "xyz", 3, xr_string_hash("xyz", 3));
    ASSERT_TRUE(xr_string_has(X, s, sub1));
    ASSERT_FALSE(xr_string_has(X, s, sub2));
    teardown();
}

TEST(string_starts_with) {
    setup();
    XrString *s = xr_string_intern(X, "hello world", 11, xr_string_hash("hello world", 11));
    XrString *prefix1 = xr_string_intern(X, "hello", 5, xr_string_hash("hello", 5));
    XrString *prefix2 = xr_string_intern(X, "world", 5, xr_string_hash("world", 5));
    ASSERT_TRUE(xr_string_starts_with(X, s, prefix1));
    ASSERT_FALSE(xr_string_starts_with(X, s, prefix2));
    teardown();
}

TEST(string_ends_with) {
    setup();
    XrString *s = xr_string_intern(X, "hello world", 11, xr_string_hash("hello world", 11));
    XrString *suffix1 = xr_string_intern(X, "world", 5, xr_string_hash("world", 5));
    XrString *suffix2 = xr_string_intern(X, "hello", 5, xr_string_hash("hello", 5));
    ASSERT_TRUE(xr_string_ends_with(X, s, suffix1));
    ASSERT_FALSE(xr_string_ends_with(X, s, suffix2));
    teardown();
}

/* ========== String Transformation Tests ========== */

TEST(string_to_upper_case) {
    setup();
    XrString *s = xr_string_intern(X, "Hello World", 11, xr_string_hash("Hello World", 11));
    XrString *upper = xr_string_to_upper_case(X, s);
    ASSERT_NOT_NULL(upper);
    ASSERT_STR_EQ(upper->data, "HELLO WORLD");
    teardown();
}

TEST(string_to_lower_case) {
    setup();
    XrString *s = xr_string_intern(X, "Hello World", 11, xr_string_hash("Hello World", 11));
    XrString *lower = xr_string_to_lower_case(X, s);
    ASSERT_NOT_NULL(lower);
    ASSERT_STR_EQ(lower->data, "hello world");
    teardown();
}

TEST(string_trim) {
    setup();
    XrString *s = xr_string_intern(X, "  hello  ", 9, xr_string_hash("  hello  ", 9));
    XrString *trimmed = xr_string_trim(X, s);
    ASSERT_NOT_NULL(trimmed);
    ASSERT_STR_EQ(trimmed->data, "hello");
    teardown();
}

TEST(string_trim_start) {
    setup();
    XrString *s = xr_string_intern(X, "  hello  ", 9, xr_string_hash("  hello  ", 9));
    XrString *trimmed = xr_string_trim_start(X, s);
    ASSERT_NOT_NULL(trimmed);
    ASSERT_STR_EQ(trimmed->data, "hello  ");
    teardown();
}

TEST(string_trim_end) {
    setup();
    XrString *s = xr_string_intern(X, "  hello  ", 9, xr_string_hash("  hello  ", 9));
    XrString *trimmed = xr_string_trim_end(X, s);
    ASSERT_NOT_NULL(trimmed);
    ASSERT_STR_EQ(trimmed->data, "  hello");
    teardown();
}

/* ========== Substring Tests ========== */

TEST(string_substring) {
    setup();
    XrString *s = xr_string_intern(X, "hello world", 11, xr_string_hash("hello world", 11));
    XrString *sub = xr_string_substring(X, s, 0, 5);
    ASSERT_NOT_NULL(sub);
    ASSERT_STR_EQ(sub->data, "hello");
    teardown();
}

TEST(string_substring_middle) {
    setup();
    XrString *s = xr_string_intern(X, "hello world", 11, xr_string_hash("hello world", 11));
    XrString *sub = xr_string_substring(X, s, 6, 11);
    ASSERT_NOT_NULL(sub);
    ASSERT_STR_EQ(sub->data, "world");
    teardown();
}

TEST(string_char_at) {
    setup();
    XrString *s = xr_string_intern(X, "hello", 5, xr_string_hash("hello", 5));
    XrString *c = xr_string_char_at(X, s, 0);
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(c->data, "h");

    c = xr_string_char_at(X, s, 4);
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(c->data, "o");
    teardown();
}

/* ========== String Replace Tests ========== */

TEST(string_replace) {
    setup();
    XrString *s = xr_string_intern(X, "hello world", 11, xr_string_hash("hello world", 11));
    XrString *old = xr_string_intern(X, "world", 5, xr_string_hash("world", 5));
    XrString *new_str = xr_string_intern(X, "xray", 4, xr_string_hash("xray", 4));
    XrString *result = xr_string_replace(X, s, old, new_str);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result->data, "hello xray");
    teardown();
}

TEST(string_replace_all) {
    setup();
    XrString *s = xr_string_intern(X, "aaa", 3, xr_string_hash("aaa", 3));
    XrString *old = xr_string_intern(X, "a", 1, xr_string_hash("a", 1));
    XrString *new_str = xr_string_intern(X, "bb", 2, xr_string_hash("bb", 2));
    XrString *result = xr_string_replace_all(X, s, old, new_str);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result->data, "bbbbbb");
    teardown();
}

/* ========== String Repeat Tests ========== */

TEST(string_repeat) {
    setup();
    XrString *s = xr_string_intern(X, "ab", 2, xr_string_hash("ab", 2));
    XrString *result = xr_string_repeat(X, s, 3);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result->data, "ababab");
    teardown();
}

TEST(string_repeat_zero) {
    setup();
    XrString *s = xr_string_intern(X, "ab", 2, xr_string_hash("ab", 2));
    XrString *result = xr_string_repeat(X, s, 0);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result->data, "");
    teardown();
}

/* ========== Unicode / UTF-8 Tests ========== */

TEST(string_utf8_length) {
    setup();
    // "你好" is 2 characters, 6 bytes in UTF-8
    XrString *s = xr_string_intern(X, "你好", 6, xr_string_hash("你好", 6));
    ASSERT_EQ_UINT(s->length, 6);                 // Byte length
    ASSERT_EQ_UINT(xr_string_char_length(s), 2);  // Character length
    teardown();
}

TEST(string_utf8_mixed) {
    setup();
    // "hello世界" is 7 characters, 11 bytes
    const char *text = "hello世界";
    size_t len = strlen(text);
    XrString *s = xr_string_intern(X, text, len, xr_string_hash(text, len));
    ASSERT_EQ_UINT(s->length, 11);                // Byte length
    ASSERT_EQ_UINT(xr_string_char_length(s), 7);  // Character length
    teardown();
}

/* ========== Hash Tests ========== */

TEST(string_hash_consistency) {
    // Same string should have same hash
    uint32_t h1 = xr_string_hash("hello", 5);
    uint32_t h2 = xr_string_hash("hello", 5);
    ASSERT_EQ_UINT(h1, h2);
}

TEST(string_hash_different) {
    uint32_t h1 = xr_string_hash("hello", 5);
    uint32_t h2 = xr_string_hash("world", 5);
    ASSERT_NE(h1, h2);
}

/* ========== Main ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("String Creation");
    RUN_TEST(string_intern_empty);
    RUN_TEST(string_intern_simple);
    RUN_TEST(string_intern_dedup);
    RUN_TEST(string_intern_different);
    RUN_TEST(string_from_int);
    RUN_TEST(string_from_int_negative);
    RUN_TEST(string_from_int_zero);
    RUN_TEST(string_from_float);

    RUN_TEST_SUITE("String Comparison");
    RUN_TEST(string_equal_same);
    RUN_TEST(string_equal_different);
    RUN_TEST(string_equal_different_length);
    RUN_TEST(string_compare_equal);
    RUN_TEST(string_compare_less);
    RUN_TEST(string_compare_greater);

    RUN_TEST_SUITE("String Concatenation");
    RUN_TEST(string_concat_basic);
    RUN_TEST(string_concat_empty_left);
    RUN_TEST(string_concat_empty_right);

    RUN_TEST_SUITE("String Methods");
    RUN_TEST(string_size);
    RUN_TEST(string_is_empty);
    RUN_TEST(string_index_of_found);
    RUN_TEST(string_index_of_not_found);
    RUN_TEST(string_has);
    RUN_TEST(string_starts_with);
    RUN_TEST(string_ends_with);

    RUN_TEST_SUITE("String Transformation");
    RUN_TEST(string_to_upper_case);
    RUN_TEST(string_to_lower_case);
    RUN_TEST(string_trim);
    RUN_TEST(string_trim_start);
    RUN_TEST(string_trim_end);

    RUN_TEST_SUITE("Substring");
    RUN_TEST(string_substring);
    RUN_TEST(string_substring_middle);
    RUN_TEST(string_char_at);

    RUN_TEST_SUITE("String Replace");
    RUN_TEST(string_replace);
    RUN_TEST(string_replace_all);

    RUN_TEST_SUITE("String Repeat");
    RUN_TEST(string_repeat);
    RUN_TEST(string_repeat_zero);

    RUN_TEST_SUITE("Unicode/UTF-8");
    RUN_TEST(string_utf8_length);
    RUN_TEST(string_utf8_mixed);

    RUN_TEST_SUITE("String Hash");
    RUN_TEST(string_hash_consistency);
    RUN_TEST(string_hash_different);
}

TEST_MAIN_BEGIN()
printf("=== xray String Unit Tests ===\n");
run_all_tests();
TEST_MAIN_END()
