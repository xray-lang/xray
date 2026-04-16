/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_regex.c - Unit tests for regex library
 *
 * KEY CONCEPT:
 *   Comprehensive tests for the regex engine covering compilation,
 *   matching, capture groups, character classes, anchors, quantifiers,
 *   replace, split, Unicode properties, and edge cases.
 */

#include "../test_framework.h"
#include "../../../stdlib/regex/xregex.h"
#include "../../../stdlib/regex/xregex_internal.h"
#include <string.h>

/* ========================================================================
 * Helper Macros
 * ======================================================================== */

#define RE_COMPILE(pattern) \
    xr_regex_compile(pattern, XR_RE_NONE, NULL)

#define RE_COMPILE_FLAGS(pattern, flags) \
    xr_regex_compile(pattern, flags, NULL)

/* ========================================================================
 * 1. Compilation Tests
 * ======================================================================== */

TEST(compile_basic) {
    XrRegex *re = RE_COMPILE("abc");
    ASSERT_NOT_NULL(re);
    ASSERT_STR_EQ(xr_regex_pattern(re), "abc");
    xr_regex_free(re);
}

TEST(compile_empty) {
    XrRegex *re = RE_COMPILE("");
    ASSERT_NOT_NULL(re);
    xr_regex_free(re);
}

TEST(compile_invalid_unmatched_paren) {
    XrRegexError err;
    XrRegex *re = xr_regex_compile("(abc", XR_RE_NONE, &err);
    ASSERT_NULL(re);
    ASSERT_NE(err, XR_RE_OK);
}

TEST(compile_invalid_unmatched_bracket) {
    XrRegexError err;
    XrRegex *re = xr_regex_compile("[abc", XR_RE_NONE, &err);
    ASSERT_NULL(re);
}

TEST(compile_invalid_bad_repeat) {
    XrRegexError err;
    XrRegex *re = xr_regex_compile("*", XR_RE_NONE, &err);
    ASSERT_NULL(re);
}

TEST(compile_flags) {
    XrRegex *re = RE_COMPILE_FLAGS("hello", XR_RE_IGNORECASE | XR_RE_MULTILINE);
    ASSERT_NOT_NULL(re);
    xr_regex_free(re);
}

TEST(compile_is_valid) {
    ASSERT_TRUE(xr_regex_is_valid("\\d+", XR_RE_NONE));
    ASSERT_TRUE(xr_regex_is_valid("[a-z]+", XR_RE_NONE));
    ASSERT_FALSE(xr_regex_is_valid("[", XR_RE_NONE));
    ASSERT_FALSE(xr_regex_is_valid("(", XR_RE_NONE));
}

/* ========================================================================
 * 2. Basic Matching Tests (test/find/full_match)
 * ======================================================================== */

TEST(test_match_simple) {
    XrRegex *re = RE_COMPILE("hello");
    ASSERT_TRUE(xr_regex_test(re, "hello world", 11));
    ASSERT_FALSE(xr_regex_test(re, "goodbye", 7));
    xr_regex_free(re);
}

TEST(test_match_digits) {
    XrRegex *re = RE_COMPILE("\\d+");
    ASSERT_TRUE(xr_regex_test(re, "abc123", 6));
    ASSERT_FALSE(xr_regex_test(re, "nodigits", 8));
    xr_regex_free(re);
}

TEST(find_position) {
    XrRegex *re = RE_COMPILE("\\d+");
    XrMatch match;
    bool found = xr_regex_match(re, "abc123def", 9, &match);
    ASSERT_TRUE(found);
    // groups[0] is full match
    ASSERT_NOT_NULL(match.groups[0].start);
    ASSERT_NOT_NULL(match.groups[0].end);
    int start = (int)(match.groups[0].start - "abc123def");
    int end = (int)(match.groups[0].end - "abc123def");
    ASSERT_EQ_INT(start, 3);
    ASSERT_EQ_INT(end, 6);
    xr_regex_free(re);
}

TEST(full_match_success) {
    XrRegex *re = RE_COMPILE("[a-z]+");
    XrMatch match;
    ASSERT_TRUE(xr_regex_full_match(re, "hello", 5, &match));
    xr_regex_free(re);
}

TEST(full_match_failure) {
    XrRegex *re = RE_COMPILE("[a-z]+");
    XrMatch match;
    ASSERT_FALSE(xr_regex_full_match(re, "hello123", 8, &match));
    xr_regex_free(re);
}

TEST(find_at_offset) {
    XrRegex *re = RE_COMPILE("\\d+");
    XrMatch match;
    // Find starting from offset 6 (past first digits)
    bool found = xr_regex_match_at(re, "abc123def456", 12, 6, &match);
    ASSERT_TRUE(found);
    int start = (int)(match.groups[0].start - "abc123def456");
    ASSERT_EQ_INT(start, 9);
    xr_regex_free(re);
}

/* ========================================================================
 * 3. Capture Group Tests
 * ======================================================================== */

TEST(capture_simple) {
    XrRegex *re = RE_COMPILE("(\\w+)@(\\w+)");
    XrMatch match;
    const char *text = "contact: user@host.com";
    bool found = xr_regex_match(re, text, (int)strlen(text), &match);
    ASSERT_TRUE(found);
    ASSERT_GE(match.group_count, 3);
    // groups[0] = full match "user@host"
    int len0 = (int)(match.groups[0].end - match.groups[0].start);
    ASSERT_EQ_INT(len0, 9);
    // groups[1] = "user"
    int len1 = (int)(match.groups[1].end - match.groups[1].start);
    ASSERT_EQ_INT(len1, 4);
    ASSERT(memcmp(match.groups[1].start, "user", 4) == 0);
    // groups[2] = "host"
    int len2 = (int)(match.groups[2].end - match.groups[2].start);
    ASSERT_EQ_INT(len2, 4);
    ASSERT(memcmp(match.groups[2].start, "host", 4) == 0);
    xr_regex_free(re);
}

TEST(capture_count) {
    XrRegex *re = RE_COMPILE("(a)(b)(c)");
    ASSERT_EQ_INT(xr_regex_capture_count(re), 3);
    xr_regex_free(re);
}

TEST(capture_nested) {
    XrRegex *re = RE_COMPILE("((a)(b))");
    ASSERT_EQ_INT(xr_regex_capture_count(re), 3);
    XrMatch match;
    bool found = xr_regex_match(re, "ab", 2, &match);
    ASSERT_TRUE(found);
    xr_regex_free(re);
}

TEST(noncapture_group) {
    XrRegex *re = RE_COMPILE("(?:ab)+");
    ASSERT_EQ_INT(xr_regex_capture_count(re), 0);
    ASSERT_TRUE(xr_regex_test(re, "abab", 4));
    xr_regex_free(re);
}

/* ========================================================================
 * 4. Character Class Tests
 * ======================================================================== */

TEST(charclass_simple) {
    XrRegex *re = RE_COMPILE("[abc]");
    ASSERT_TRUE(xr_regex_test(re, "a", 1));
    ASSERT_TRUE(xr_regex_test(re, "b", 1));
    ASSERT_TRUE(xr_regex_test(re, "c", 1));
    ASSERT_FALSE(xr_regex_test(re, "d", 1));
    xr_regex_free(re);
}

TEST(charclass_range) {
    XrRegex *re = RE_COMPILE("[a-z]");
    ASSERT_TRUE(xr_regex_test(re, "a", 1));
    ASSERT_TRUE(xr_regex_test(re, "m", 1));
    ASSERT_TRUE(xr_regex_test(re, "z", 1));
    ASSERT_FALSE(xr_regex_test(re, "A", 1));
    ASSERT_FALSE(xr_regex_test(re, "0", 1));
    xr_regex_free(re);
}

TEST(charclass_negated) {
    XrRegex *re = RE_COMPILE("[^a-z]");
    ASSERT_TRUE(xr_regex_test(re, "A", 1));
    ASSERT_TRUE(xr_regex_test(re, "0", 1));
    ASSERT_FALSE(xr_regex_test(re, "a", 1));
    ASSERT_FALSE(xr_regex_test(re, "z", 1));
    xr_regex_free(re);
}

TEST(charclass_combined) {
    XrRegex *re = RE_COMPILE("[a-zA-Z0-9]");
    ASSERT_TRUE(xr_regex_test(re, "a", 1));
    ASSERT_TRUE(xr_regex_test(re, "Z", 1));
    ASSERT_TRUE(xr_regex_test(re, "5", 1));
    ASSERT_FALSE(xr_regex_test(re, "-", 1));
    xr_regex_free(re);
}

TEST(predefined_digit) {
    XrRegex *re = RE_COMPILE("\\d");
    ASSERT_TRUE(xr_regex_test(re, "0", 1));
    ASSERT_TRUE(xr_regex_test(re, "9", 1));
    ASSERT_FALSE(xr_regex_test(re, "a", 1));
    xr_regex_free(re);
}

TEST(predefined_word) {
    XrRegex *re = RE_COMPILE("\\w");
    ASSERT_TRUE(xr_regex_test(re, "a", 1));
    ASSERT_TRUE(xr_regex_test(re, "Z", 1));
    ASSERT_TRUE(xr_regex_test(re, "0", 1));
    ASSERT_TRUE(xr_regex_test(re, "_", 1));
    ASSERT_FALSE(xr_regex_test(re, "-", 1));
    xr_regex_free(re);
}

TEST(predefined_space) {
    XrRegex *re = RE_COMPILE("\\s");
    ASSERT_TRUE(xr_regex_test(re, " ", 1));
    ASSERT_TRUE(xr_regex_test(re, "\t", 1));
    ASSERT_TRUE(xr_regex_test(re, "\n", 1));
    ASSERT_FALSE(xr_regex_test(re, "a", 1));
    xr_regex_free(re);
}

TEST(predefined_negated) {
    XrRegex *re_D = RE_COMPILE("\\D");
    ASSERT_TRUE(xr_regex_test(re_D, "a", 1));
    ASSERT_FALSE(xr_regex_test(re_D, "0", 1));
    xr_regex_free(re_D);

    XrRegex *re_W = RE_COMPILE("\\W");
    ASSERT_TRUE(xr_regex_test(re_W, "-", 1));
    ASSERT_FALSE(xr_regex_test(re_W, "a", 1));
    xr_regex_free(re_W);

    XrRegex *re_S = RE_COMPILE("\\S");
    ASSERT_TRUE(xr_regex_test(re_S, "a", 1));
    ASSERT_FALSE(xr_regex_test(re_S, " ", 1));
    xr_regex_free(re_S);
}

/* ========================================================================
 * 5. Anchor Tests
 * ======================================================================== */

TEST(anchor_begin) {
    XrRegex *re = RE_COMPILE("^abc");
    ASSERT_TRUE(xr_regex_test(re, "abc", 3));
    ASSERT_TRUE(xr_regex_test(re, "abcdef", 6));
    ASSERT_FALSE(xr_regex_test(re, "xabc", 4));
    xr_regex_free(re);
}

TEST(anchor_end) {
    XrRegex *re = RE_COMPILE("abc$");
    ASSERT_TRUE(xr_regex_test(re, "abc", 3));
    ASSERT_TRUE(xr_regex_test(re, "xabc", 4));
    ASSERT_FALSE(xr_regex_test(re, "abcx", 4));
    xr_regex_free(re);
}

TEST(anchor_both) {
    XrRegex *re = RE_COMPILE("^abc$");
    ASSERT_TRUE(xr_regex_test(re, "abc", 3));
    ASSERT_FALSE(xr_regex_test(re, "abcd", 4));
    ASSERT_FALSE(xr_regex_test(re, "xabc", 4));
    xr_regex_free(re);
}

TEST(word_boundary) {
    XrRegex *re = RE_COMPILE("\\bfoo\\b");
    ASSERT_TRUE(xr_regex_test(re, "foo", 3));
    ASSERT_TRUE(xr_regex_test(re, "foo bar", 7));
    ASSERT_FALSE(xr_regex_test(re, "foobar", 6));
    ASSERT_FALSE(xr_regex_test(re, "barfoo", 6));
    xr_regex_free(re);
}

TEST(not_word_boundary) {
    XrRegex *re = RE_COMPILE("\\Bfoo");
    ASSERT_TRUE(xr_regex_test(re, "xfoo", 4));
    ASSERT_FALSE(xr_regex_test(re, "foo", 3));
    xr_regex_free(re);
}

/* ========================================================================
 * 6. Quantifier Tests
 * ======================================================================== */

TEST(quantifier_star) {
    XrRegex *re = RE_COMPILE("a*");
    ASSERT_TRUE(xr_regex_test(re, "", 0));
    ASSERT_TRUE(xr_regex_test(re, "aaa", 3));
    xr_regex_free(re);
}

TEST(quantifier_plus) {
    XrRegex *re = RE_COMPILE("a+");
    ASSERT_FALSE(xr_regex_test(re, "", 0));
    ASSERT_TRUE(xr_regex_test(re, "a", 1));
    ASSERT_TRUE(xr_regex_test(re, "aaa", 3));
    ASSERT_FALSE(xr_regex_test(re, "b", 1));
    xr_regex_free(re);
}

TEST(quantifier_question) {
    XrRegex *re = RE_COMPILE("a?");
    ASSERT_TRUE(xr_regex_test(re, "", 0));
    ASSERT_TRUE(xr_regex_test(re, "a", 1));
    xr_regex_free(re);
}

TEST(quantifier_exact) {
    XrRegex *re = RE_COMPILE("a{3}");
    ASSERT_FALSE(xr_regex_test(re, "aa", 2));
    ASSERT_TRUE(xr_regex_test(re, "aaa", 3));
    ASSERT_TRUE(xr_regex_test(re, "aaaa", 4));
    xr_regex_free(re);
}

TEST(quantifier_range) {
    XrRegex *re = RE_COMPILE("a{2,4}");
    ASSERT_FALSE(xr_regex_test(re, "a", 1));
    ASSERT_TRUE(xr_regex_test(re, "aa", 2));
    ASSERT_TRUE(xr_regex_test(re, "aaa", 3));
    ASSERT_TRUE(xr_regex_test(re, "aaaa", 4));
    xr_regex_free(re);
}

TEST(quantifier_min) {
    XrRegex *re = RE_COMPILE("a{2,}");
    ASSERT_FALSE(xr_regex_test(re, "a", 1));
    ASSERT_TRUE(xr_regex_test(re, "aa", 2));
    ASSERT_TRUE(xr_regex_test(re, "aaaaa", 5));
    xr_regex_free(re);
}

/* ========================================================================
 * 7. Greedy vs Non-greedy Tests
 * ======================================================================== */

TEST(greedy_plus) {
    XrRegex *re = RE_COMPILE("a+");
    XrMatch match;
    bool found = xr_regex_match(re, "aaa", 3, &match);
    ASSERT_TRUE(found);
    int len = (int)(match.groups[0].end - match.groups[0].start);
    ASSERT_EQ_INT(len, 3);
    xr_regex_free(re);
}

TEST(lazy_plus) {
    XrRegex *re = RE_COMPILE("a+?");
    XrMatch match;
    bool found = xr_regex_match(re, "aaa", 3, &match);
    ASSERT_TRUE(found);
    int len = (int)(match.groups[0].end - match.groups[0].start);
    ASSERT_EQ_INT(len, 1);
    xr_regex_free(re);
}

TEST(lazy_star) {
    XrRegex *re = RE_COMPILE("a*?");
    XrMatch match;
    bool found = xr_regex_match(re, "aaa", 3, &match);
    ASSERT_TRUE(found);
    int len = (int)(match.groups[0].end - match.groups[0].start);
    ASSERT_EQ_INT(len, 0);
    xr_regex_free(re);
}

TEST(lazy_repeat) {
    XrRegex *re = RE_COMPILE("a{2,}?");
    XrMatch match;
    bool found = xr_regex_match(re, "aaaa", 4, &match);
    ASSERT_TRUE(found);
    int len = (int)(match.groups[0].end - match.groups[0].start);
    ASSERT_EQ_INT(len, 2);
    xr_regex_free(re);
}

/* ========================================================================
 * 8. Alternation Tests
 * ======================================================================== */

TEST(alternation_simple) {
    XrRegex *re = RE_COMPILE("a|b");
    ASSERT_TRUE(xr_regex_test(re, "a", 1));
    ASSERT_TRUE(xr_regex_test(re, "b", 1));
    ASSERT_FALSE(xr_regex_test(re, "c", 1));
    xr_regex_free(re);
}

TEST(alternation_words) {
    XrRegex *re = RE_COMPILE("foo|bar|baz");
    ASSERT_TRUE(xr_regex_test(re, "foo", 3));
    ASSERT_TRUE(xr_regex_test(re, "bar", 3));
    ASSERT_TRUE(xr_regex_test(re, "baz", 3));
    ASSERT_FALSE(xr_regex_test(re, "qux", 3));
    xr_regex_free(re);
}

TEST(alternation_in_group) {
    XrRegex *re = RE_COMPILE("(a|b)c");
    ASSERT_TRUE(xr_regex_test(re, "ac", 2));
    ASSERT_TRUE(xr_regex_test(re, "bc", 2));
    ASSERT_FALSE(xr_regex_test(re, "cc", 2));
    xr_regex_free(re);
}

/* ========================================================================
 * 9. Dot Tests
 * ======================================================================== */

TEST(dot_basic) {
    XrRegex *re = RE_COMPILE("a.b");
    ASSERT_TRUE(xr_regex_test(re, "aXb", 3));
    ASSERT_TRUE(xr_regex_test(re, "a b", 3));
    ASSERT_FALSE(xr_regex_test(re, "a\nb", 3));
    xr_regex_free(re);
}

TEST(dot_dotall) {
    XrRegex *re = RE_COMPILE_FLAGS("a.b", XR_RE_DOTALL);
    ASSERT_TRUE(xr_regex_test(re, "a\nb", 3));
    xr_regex_free(re);
}

/* ========================================================================
 * 10. Ignore Case Tests
 * ======================================================================== */

TEST(ignorecase_basic) {
    XrRegex *re = RE_COMPILE_FLAGS("hello", XR_RE_IGNORECASE);
    ASSERT_TRUE(xr_regex_test(re, "HELLO", 5));
    ASSERT_TRUE(xr_regex_test(re, "HeLLo", 5));
    ASSERT_TRUE(xr_regex_test(re, "hello", 5));
    xr_regex_free(re);
}

TEST(ignorecase_match_text) {
    XrRegex *re = RE_COMPILE_FLAGS("hello", XR_RE_IGNORECASE);
    XrMatch match;
    bool found = xr_regex_match(re, "Say HELLO there", 15, &match);
    ASSERT_TRUE(found);
    int len = (int)(match.groups[0].end - match.groups[0].start);
    ASSERT_EQ_INT(len, 5);
    ASSERT(memcmp(match.groups[0].start, "HELLO", 5) == 0);
    xr_regex_free(re);
}

TEST(case_sensitive) {
    XrRegex *re = RE_COMPILE("hello");
    ASSERT_FALSE(xr_regex_test(re, "HELLO", 5));
    xr_regex_free(re);
}

/* ========================================================================
 * 11. Multiline Mode Tests
 * ======================================================================== */

TEST(multiline_caret) {
    XrRegex *re = RE_COMPILE_FLAGS("^world", XR_RE_MULTILINE);
    ASSERT_TRUE(xr_regex_test(re, "hello\nworld", 11));
    xr_regex_free(re);
}

TEST(multiline_dollar) {
    XrRegex *re = RE_COMPILE_FLAGS("hello$", XR_RE_MULTILINE);
    ASSERT_TRUE(xr_regex_test(re, "hello\nworld", 11));
    xr_regex_free(re);
}

/* ========================================================================
 * 12. Escape Sequence Tests
 * ======================================================================== */

TEST(escape_metachar) {
    XrRegex *re = RE_COMPILE("\\.");
    ASSERT_TRUE(xr_regex_test(re, ".", 1));
    ASSERT_FALSE(xr_regex_test(re, "a", 1));
    xr_regex_free(re);
}

TEST(escape_backslash) {
    XrRegex *re = RE_COMPILE("\\\\");
    ASSERT_TRUE(xr_regex_test(re, "\\", 1));
    xr_regex_free(re);
}

TEST(escape_tab_newline) {
    XrRegex *re_t = RE_COMPILE("\\t");
    ASSERT_TRUE(xr_regex_test(re_t, "\t", 1));
    xr_regex_free(re_t);

    XrRegex *re_n = RE_COMPILE("\\n");
    ASSERT_TRUE(xr_regex_test(re_n, "\n", 1));
    xr_regex_free(re_n);
}

/* ========================================================================
 * 13. Replace Tests
 * ======================================================================== */

TEST(replace_first) {
    XrRegex *re = RE_COMPILE("\\d+");
    char *result = xr_regex_replace_alloc(re, "abc123def456", 12, "X", false);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result, "abcXdef456");
    free(result);
    xr_regex_free(re);
}

TEST(replace_all) {
    XrRegex *re = RE_COMPILE("\\d+");
    char *result = xr_regex_replace_alloc(re, "a1b2c3", 6, "X", true);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result, "aXbXcX");
    free(result);
    xr_regex_free(re);
}

TEST(replace_alloc) {
    XrRegex *re = RE_COMPILE("\\s+");
    char *result = xr_regex_replace_alloc(re, "a   b   c", 9, "-", true);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result, "a-b-c");
    free(result);
    xr_regex_free(re);
}

TEST(replace_with_capture_ref) {
    XrRegex *re = RE_COMPILE("(\\w+)-(\\w+)");
    char *result = xr_regex_replace_alloc(re, "hello-world", 11, "$2-$1", false);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result, "world-hello");
    free(result);
    xr_regex_free(re);
}

TEST(replace_no_match) {
    XrRegex *re = RE_COMPILE("xyz");
    char *result = xr_regex_replace_alloc(re, "hello", 5, "X", false);
    // No match -> returns copy of original text
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result, "hello");
    free(result);
    xr_regex_free(re);
}

/* ========================================================================
 * 14. Split Tests
 * ======================================================================== */

TEST(split_basic) {
    XrRegex *re = RE_COMPILE(",\\s*");
    XrSplitPart parts[32];
    int count = xr_regex_split(re, "a, b, c, d", 10, parts, 32, -1);
    ASSERT_EQ_INT(count, 4);
    ASSERT(memcmp(parts[0].str, "a", 1) == 0);
    ASSERT_EQ_INT(parts[0].len, 1);
    ASSERT(memcmp(parts[1].str, "b", 1) == 0);
    ASSERT_EQ_INT(parts[1].len, 1);
    ASSERT(memcmp(parts[3].str, "d", 1) == 0);
    ASSERT_EQ_INT(parts[3].len, 1);
    xr_regex_free(re);
}

TEST(split_no_match) {
    XrRegex *re = RE_COMPILE("xyz");
    XrSplitPart parts[32];
    int count = xr_regex_split(re, "hello", 5, parts, 32, -1);
    ASSERT_EQ_INT(count, 1);
    ASSERT(memcmp(parts[0].str, "hello", 5) == 0);
    xr_regex_free(re);
}

TEST(split_with_limit) {
    XrRegex *re = RE_COMPILE(",");
    XrSplitPart parts[32];
    int count = xr_regex_split(re, "a,b,c,d", 7, parts, 32, 2);
    ASSERT_EQ_INT(count, 2);
    xr_regex_free(re);
}

/* ========================================================================
 * 15. Count and FindAll Tests
 * ======================================================================== */

TEST(count_basic) {
    XrRegex *re = RE_COMPILE("\\d+");
    int count = xr_regex_count(re, "a1b2c3d4", 8);
    ASSERT_EQ_INT(count, 4);
    xr_regex_free(re);
}

TEST(count_no_match) {
    XrRegex *re = RE_COMPILE("\\d+");
    int count = xr_regex_count(re, "hello", 5);
    ASSERT_EQ_INT(count, 0);
    xr_regex_free(re);
}

TEST(find_all_basic) {
    XrRegex *re = RE_COMPILE("\\d+");
    int count = 0;
    XrMatch *matches = xr_regex_find_all(re, "a1b22c333", 9, -1, &count);
    ASSERT_EQ_INT(count, 3);
    ASSERT_NOT_NULL(matches);
    // First match: "1"
    int len0 = (int)(matches[0].groups[0].end - matches[0].groups[0].start);
    ASSERT_EQ_INT(len0, 1);
    // Second match: "22"
    int len1 = (int)(matches[1].groups[0].end - matches[1].groups[0].start);
    ASSERT_EQ_INT(len1, 2);
    // Third match: "333"
    int len2 = (int)(matches[2].groups[0].end - matches[2].groups[0].start);
    ASSERT_EQ_INT(len2, 3);
    xr_regex_find_all_free(matches);
    xr_regex_free(re);
}

/* ========================================================================
 * 16. Escape Utility Tests
 * ======================================================================== */

TEST(escape_special_chars) {
    char buf[256];
    int n = xr_regex_escape("a.b*c?", 6, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    // Should contain escaped chars
    ASSERT_NOT_NULL(strstr(buf, "\\."));
    ASSERT_NOT_NULL(strstr(buf, "\\*"));
    ASSERT_NOT_NULL(strstr(buf, "\\?"));
}

/* ========================================================================
 * 17. Unicode Property Tests
 * ======================================================================== */

TEST(unicode_han) {
    XrRegex *re = RE_COMPILE("\\p{Han}+");
    ASSERT_NOT_NULL(re);
    // "你好" = 6 bytes UTF-8
    ASSERT_TRUE(xr_regex_test(re, "\xe4\xbd\xa0\xe5\xa5\xbd", 6));
    // Latin only
    ASSERT_FALSE(xr_regex_test(re, "hello", 5));
    xr_regex_free(re);
}

TEST(unicode_latin) {
    XrRegex *re = RE_COMPILE("\\p{Latin}+");
    ASSERT_NOT_NULL(re);
    ASSERT_TRUE(xr_regex_test(re, "hello", 5));
    xr_regex_free(re);
}

TEST(unicode_number) {
    XrRegex *re = RE_COMPILE("\\p{N}+");
    ASSERT_NOT_NULL(re);
    ASSERT_TRUE(xr_regex_test(re, "123", 3));
    ASSERT_FALSE(xr_regex_test(re, "abc", 3));
    xr_regex_free(re);
}

TEST(unicode_negated) {
    XrRegex *re = RE_COMPILE("\\P{Han}+");
    ASSERT_NOT_NULL(re);
    ASSERT_TRUE(xr_regex_test(re, "hello", 5));
    xr_regex_free(re);
}

/* ========================================================================
 * 18. Edge Cases
 * ======================================================================== */

TEST(empty_pattern_empty_text) {
    XrRegex *re = RE_COMPILE("");
    ASSERT_TRUE(xr_regex_test(re, "", 0));
    xr_regex_free(re);
}

TEST(empty_pattern_nonempty_text) {
    XrRegex *re = RE_COMPILE("");
    ASSERT_TRUE(xr_regex_test(re, "abc", 3));
    xr_regex_free(re);
}

TEST(deeply_nested_groups) {
    XrRegex *re = RE_COMPILE("((((((((((x))))))))))");
    ASSERT_NOT_NULL(re);
    ASSERT_TRUE(xr_regex_test(re, "x", 1));
    xr_regex_free(re);
}

TEST(complex_pattern) {
    XrRegex *re = RE_COMPILE("(a+|b)+");
    ASSERT_TRUE(xr_regex_test(re, "ab", 2));
    ASSERT_TRUE(xr_regex_test(re, "aab", 3));
    xr_regex_free(re);
}

TEST(error_string) {
    const char *msg = xr_regex_error_str(XR_RE_ERR_SYNTAX);
    ASSERT_NOT_NULL(msg);
    ASSERT_GT((int)strlen(msg), 0);
}

/* ========================================================================
 * 19. Iterator Tests
 * ======================================================================== */

TEST(iterator_basic) {
    XrRegex *re = RE_COMPILE("\\d+");
    XrMatchIter *iter = xr_regex_iter_new(re, "a1b22c333", 9);
    ASSERT_NOT_NULL(iter);

    XrMatch match;
    int count = 0;
    while (xr_regex_iter_next(iter, &match)) {
        count++;
    }
    ASSERT_EQ_INT(count, 3);

    xr_regex_iter_free(iter);
    xr_regex_free(re);
}

TEST(iterator_reset) {
    XrRegex *re = RE_COMPILE("\\w+");
    XrMatchIter *iter = xr_regex_iter_new(re, "a b c", 5);
    ASSERT_NOT_NULL(iter);

    XrMatch match;
    int count = 0;
    while (xr_regex_iter_next(iter, &match)) count++;
    ASSERT_EQ_INT(count, 3);

    // Reset and iterate again
    xr_regex_iter_reset(iter);
    count = 0;
    while (xr_regex_iter_next(iter, &match)) count++;
    ASSERT_EQ_INT(count, 3);

    xr_regex_iter_free(iter);
    xr_regex_free(re);
}

/* ========================================================================
 * 20. Inline Flags Tests
 * ======================================================================== */

TEST(inline_ignorecase) {
    XrRegex *re = RE_COMPILE("(?i)hello");
    ASSERT_NOT_NULL(re);
    ASSERT_TRUE(xr_regex_test(re, "HELLO", 5));
    xr_regex_free(re);
}

TEST(inline_multiline) {
    XrRegex *re = RE_COMPILE("(?m)^world");
    ASSERT_NOT_NULL(re);
    ASSERT_TRUE(xr_regex_test(re, "hello\nworld", 11));
    xr_regex_free(re);
}

TEST(inline_dotall) {
    XrRegex *re = RE_COMPILE("(?s)a.b");
    ASSERT_NOT_NULL(re);
    ASSERT_TRUE(xr_regex_test(re, "a\nb", 3));
    xr_regex_free(re);
}

TEST(inline_combined) {
    XrRegex *re = RE_COMPILE("(?im)^hello");
    ASSERT_NOT_NULL(re);
    ASSERT_TRUE(xr_regex_test(re, "world\nHELLO", 11));
    xr_regex_free(re);
}

/* ========================================================================
 * Main
 * ======================================================================== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("Compilation");
    RUN_TEST(compile_basic);
    RUN_TEST(compile_empty);
    RUN_TEST(compile_invalid_unmatched_paren);
    RUN_TEST(compile_invalid_unmatched_bracket);
    RUN_TEST(compile_invalid_bad_repeat);
    RUN_TEST(compile_flags);
    RUN_TEST(compile_is_valid);

    RUN_TEST_SUITE("Basic Matching");
    RUN_TEST(test_match_simple);
    RUN_TEST(test_match_digits);
    RUN_TEST(find_position);
    RUN_TEST(full_match_success);
    RUN_TEST(full_match_failure);
    RUN_TEST(find_at_offset);

    RUN_TEST_SUITE("Capture Groups");
    RUN_TEST(capture_simple);
    RUN_TEST(capture_count);
    RUN_TEST(capture_nested);
    RUN_TEST(noncapture_group);

    RUN_TEST_SUITE("Character Classes");
    RUN_TEST(charclass_simple);
    RUN_TEST(charclass_range);
    RUN_TEST(charclass_negated);
    RUN_TEST(charclass_combined);
    RUN_TEST(predefined_digit);
    RUN_TEST(predefined_word);
    RUN_TEST(predefined_space);
    RUN_TEST(predefined_negated);

    RUN_TEST_SUITE("Anchors");
    RUN_TEST(anchor_begin);
    RUN_TEST(anchor_end);
    RUN_TEST(anchor_both);
    RUN_TEST(word_boundary);
    RUN_TEST(not_word_boundary);

    RUN_TEST_SUITE("Quantifiers");
    RUN_TEST(quantifier_star);
    RUN_TEST(quantifier_plus);
    RUN_TEST(quantifier_question);
    RUN_TEST(quantifier_exact);
    RUN_TEST(quantifier_range);
    RUN_TEST(quantifier_min);

    RUN_TEST_SUITE("Greedy vs Non-greedy");
    RUN_TEST(greedy_plus);
    RUN_TEST(lazy_plus);
    RUN_TEST(lazy_star);
    RUN_TEST(lazy_repeat);

    RUN_TEST_SUITE("Alternation");
    RUN_TEST(alternation_simple);
    RUN_TEST(alternation_words);
    RUN_TEST(alternation_in_group);

    RUN_TEST_SUITE("Dot");
    RUN_TEST(dot_basic);
    RUN_TEST(dot_dotall);

    RUN_TEST_SUITE("Ignore Case");
    RUN_TEST(ignorecase_basic);
    RUN_TEST(ignorecase_match_text);
    RUN_TEST(case_sensitive);

    RUN_TEST_SUITE("Multiline Mode");
    RUN_TEST(multiline_caret);
    RUN_TEST(multiline_dollar);

    RUN_TEST_SUITE("Escape Sequences");
    RUN_TEST(escape_metachar);
    RUN_TEST(escape_backslash);
    RUN_TEST(escape_tab_newline);

    RUN_TEST_SUITE("Replace");
    RUN_TEST(replace_first);
    RUN_TEST(replace_all);
    RUN_TEST(replace_alloc);
    RUN_TEST(replace_with_capture_ref);
    RUN_TEST(replace_no_match);

    RUN_TEST_SUITE("Split");
    RUN_TEST(split_basic);
    RUN_TEST(split_no_match);
    RUN_TEST(split_with_limit);

    RUN_TEST_SUITE("Count and FindAll");
    RUN_TEST(count_basic);
    RUN_TEST(count_no_match);
    RUN_TEST(find_all_basic);

    RUN_TEST_SUITE("Escape Utility");
    RUN_TEST(escape_special_chars);

    RUN_TEST_SUITE("Unicode Properties");
    RUN_TEST(unicode_han);
    RUN_TEST(unicode_latin);
    RUN_TEST(unicode_number);
    RUN_TEST(unicode_negated);

    RUN_TEST_SUITE("Edge Cases");
    RUN_TEST(empty_pattern_empty_text);
    RUN_TEST(empty_pattern_nonempty_text);
    RUN_TEST(deeply_nested_groups);
    RUN_TEST(complex_pattern);
    RUN_TEST(error_string);

    RUN_TEST_SUITE("Iterator");
    RUN_TEST(iterator_basic);
    RUN_TEST(iterator_reset);

    RUN_TEST_SUITE("Inline Flags");
    RUN_TEST(inline_ignorecase);
    RUN_TEST(inline_multiline);
    RUN_TEST(inline_dotall);
    RUN_TEST(inline_combined);

TEST_MAIN_END()
