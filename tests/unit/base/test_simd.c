/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_simd.c - Unit tests for SIMD batch scanning and character tables
 *
 * KEY CONCEPT:
 *   Tests character classification lookup tables, hex/digit conversion,
 *   SIMD string search, whitespace skipping, and typed array memset.
 */

#include "../test_framework.h"
#include "base/xsimd.h"
#include <string.h>

/* ========== Character Classification Tables ========== */

TEST(simd_char_class_digit) {
    for (char c = '0'; c <= '9'; c++) {
        ASSERT_TRUE(XR_IS_DIGIT(c));
    }
    ASSERT_FALSE(XR_IS_DIGIT('a'));
    ASSERT_FALSE(XR_IS_DIGIT(' '));
    ASSERT_FALSE(XR_IS_DIGIT('\0'));
}

TEST(simd_char_class_alpha) {
    for (char c = 'A'; c <= 'Z'; c++) {
        ASSERT_TRUE(XR_IS_ALPHA(c));
    }
    for (char c = 'a'; c <= 'z'; c++) {
        ASSERT_TRUE(XR_IS_ALPHA(c));
    }
    ASSERT_FALSE(XR_IS_ALPHA('0'));
    ASSERT_FALSE(XR_IS_ALPHA(' '));
}

TEST(simd_char_class_alnum) {
    ASSERT_TRUE(XR_IS_ALNUM('A'));
    ASSERT_TRUE(XR_IS_ALNUM('z'));
    ASSERT_TRUE(XR_IS_ALNUM('0'));
    ASSERT_TRUE(XR_IS_ALNUM('9'));
    ASSERT_FALSE(XR_IS_ALNUM(' '));
    ASSERT_FALSE(XR_IS_ALNUM('!'));
}

TEST(simd_char_class_hex) {
    for (char c = '0'; c <= '9'; c++) ASSERT_TRUE(XR_IS_HEX(c));
    for (char c = 'a'; c <= 'f'; c++) ASSERT_TRUE(XR_IS_HEX(c));
    for (char c = 'A'; c <= 'F'; c++) ASSERT_TRUE(XR_IS_HEX(c));
    ASSERT_FALSE(XR_IS_HEX('g'));
    ASSERT_FALSE(XR_IS_HEX('G'));
    ASSERT_FALSE(XR_IS_HEX(' '));
}

TEST(simd_char_class_ws) {
    ASSERT_TRUE(XR_IS_WS(' '));
    ASSERT_TRUE(XR_IS_WS('\t'));
    ASSERT_FALSE(XR_IS_WS('\n'));  // newline is separate
    ASSERT_FALSE(XR_IS_WS('a'));
}

TEST(simd_char_class_newline) {
    ASSERT_TRUE(XR_IS_NEWLINE('\n'));
    ASSERT_TRUE(XR_IS_NEWLINE('\r'));
    ASSERT_FALSE(XR_IS_NEWLINE(' '));
    ASSERT_FALSE(XR_IS_NEWLINE('a'));
}

TEST(simd_char_class_whitespace) {
    // XR_IS_WHITESPACE = WS | NEWLINE
    ASSERT_TRUE(XR_IS_WHITESPACE(' '));
    ASSERT_TRUE(XR_IS_WHITESPACE('\t'));
    ASSERT_TRUE(XR_IS_WHITESPACE('\n'));
    ASSERT_TRUE(XR_IS_WHITESPACE('\r'));
    ASSERT_FALSE(XR_IS_WHITESPACE('a'));
}

TEST(simd_char_class_ident) {
    ASSERT_TRUE(XR_IS_IDENT('a'));
    ASSERT_TRUE(XR_IS_IDENT('Z'));
    ASSERT_TRUE(XR_IS_IDENT('_'));
    ASSERT_TRUE(XR_IS_IDENT('0'));
    ASSERT_FALSE(XR_IS_IDENT(' '));
    ASSERT_FALSE(XR_IS_IDENT('!'));
}

/* ========== Hex Conversion Table ========== */

TEST(simd_hex_to_val) {
    ASSERT_EQ_INT(XR_HEX_TO_VAL('0'), 0);
    ASSERT_EQ_INT(XR_HEX_TO_VAL('9'), 9);
    ASSERT_EQ_INT(XR_HEX_TO_VAL('a'), 10);
    ASSERT_EQ_INT(XR_HEX_TO_VAL('f'), 15);
    ASSERT_EQ_INT(XR_HEX_TO_VAL('A'), 10);
    ASSERT_EQ_INT(XR_HEX_TO_VAL('F'), 15);
}

TEST(simd_digit_to_val) {
    ASSERT_EQ_INT(XR_DIGIT_TO_VAL('0'), 0);
    ASSERT_EQ_INT(XR_DIGIT_TO_VAL('5'), 5);
    ASSERT_EQ_INT(XR_DIGIT_TO_VAL('9'), 9);
}

/* ========== SIMD Detection ========== */

TEST(simd_detect) {
    XrSimdLevel level = xr_simd_detect();
    ASSERT_TRUE(level >= XR_SIMD_NONE && level <= XR_SIMD_AVX512);
}

/* ========== SIMD Find Functions ========== */

TEST(simd_find_char) {
    const char *s = "hello world!";
    size_t len = strlen(s);

    const char *found = xr_simd_find_char(s, len, 'w');
    ASSERT_NOT_NULL(found);
    ASSERT_EQ_PTR(found, s + 6);

    // Not found: returns pointer past end (s + len)
    const char *nf = xr_simd_find_char(s, len, 'z');
    ASSERT_EQ_PTR(nf, s + len);
}

TEST(simd_find_char_first) {
    const char *s = "aabaa";
    const char *found = xr_simd_find_char(s, 5, 'b');
    ASSERT_NOT_NULL(found);
    ASSERT_EQ_PTR(found, s + 2);
}

TEST(simd_skip_ws) {
    const char *s = "   \thello";
    size_t len = strlen(s);

    const char *result = xr_simd_skip_ws(s, len);
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_PTR(result, s + 4);  // skip "   \t"
}

TEST(simd_skip_whitespace) {
    const char *s = "  \n\t\r hello";
    size_t len = strlen(s);

    const char *result = xr_simd_skip_whitespace(s, len);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(*result == 'h');
}

TEST(simd_find_newline) {
    const char *s = "first line\nsecond";
    size_t len = strlen(s);

    const char *result = xr_simd_find_newline(s, len);
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_PTR(result, s + 10);  // position of '\n'
}

TEST(simd_find_string_end) {
    const char *s = "hello\\nworld\"rest";
    size_t len = strlen(s);

    // Should find either backslash or double-quote
    const char *result = xr_simd_find_string_end(s, len);
    ASSERT_NOT_NULL(result);
}

/* ========== SIMD Memset ========== */

TEST(simd_memset32) {
    uint32_t buf[16];
    xr_simd_memset32(buf, 0xDEADBEEF, 16);
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ_UINT(buf[i], 0xDEADBEEF);
    }
}

TEST(simd_memset64) {
    uint64_t buf[8];
    xr_simd_memset64(buf, 0x123456789ABCDEF0ULL, 8);
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ_UINT(buf[i], 0x123456789ABCDEF0ULL);
    }
}

TEST(simd_memset_f32) {
    float buf[8];
    xr_simd_memset_f32(buf, 3.14f, 8);
    for (int i = 0; i < 8; i++) {
        ASSERT_FLOAT_EQ(buf[i], 3.14f, 0.001);
    }
}

TEST(simd_memset_f64) {
    double buf[4];
    xr_simd_memset_f64(buf, 2.718281828, 4);
    for (int i = 0; i < 4; i++) {
        ASSERT_FLOAT_EQ(buf[i], 2.718281828, 0.000001);
    }
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("SIMD - Character Classification");
    RUN_TEST(simd_char_class_digit);
    RUN_TEST(simd_char_class_alpha);
    RUN_TEST(simd_char_class_alnum);
    RUN_TEST(simd_char_class_hex);
    RUN_TEST(simd_char_class_ws);
    RUN_TEST(simd_char_class_newline);
    RUN_TEST(simd_char_class_whitespace);
    RUN_TEST(simd_char_class_ident);

    RUN_TEST_SUITE("SIMD - Conversion Tables");
    RUN_TEST(simd_hex_to_val);
    RUN_TEST(simd_digit_to_val);

    RUN_TEST_SUITE("SIMD - Detection");
    RUN_TEST(simd_detect);

    RUN_TEST_SUITE("SIMD - Find Functions");
    RUN_TEST(simd_find_char);
    RUN_TEST(simd_find_char_first);
    RUN_TEST(simd_skip_ws);
    RUN_TEST(simd_skip_whitespace);
    RUN_TEST(simd_find_newline);
    RUN_TEST(simd_find_string_end);

    RUN_TEST_SUITE("SIMD - Memset");
    RUN_TEST(simd_memset32);
    RUN_TEST(simd_memset64);
    RUN_TEST(simd_memset_f32);
    RUN_TEST(simd_memset_f64);

TEST_MAIN_END()
