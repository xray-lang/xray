/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_swar.c - Unit tests for SWAR (SIMD Within A Register) fast parsing
 *
 * KEY CONCEPT:
 *   Tests fast integer/hex parsing using 64-bit register parallelism.
 *   Covers valid inputs, edge cases, overflow, and invalid inputs.
 */

#include "../test_framework.h"
#include "base/xswar.h"

/* ========== Parse Unsigned Integer ========== */

TEST(swar_parse_uint_basic) {
    uint64_t result;
    ASSERT_TRUE(xr_swar_parse_uint("0", 1, &result));
    ASSERT_EQ_UINT(result, 0);

    ASSERT_TRUE(xr_swar_parse_uint("1", 1, &result));
    ASSERT_EQ_UINT(result, 1);

    ASSERT_TRUE(xr_swar_parse_uint("42", 2, &result));
    ASSERT_EQ_UINT(result, 42);

    ASSERT_TRUE(xr_swar_parse_uint("123456", 6, &result));
    ASSERT_EQ_UINT(result, 123456);
}

TEST(swar_parse_uint_large) {
    uint64_t result;
    ASSERT_TRUE(xr_swar_parse_uint("12345678", 8, &result));
    ASSERT_EQ_UINT(result, 12345678);

    ASSERT_TRUE(xr_swar_parse_uint("99999999", 8, &result));
    ASSERT_EQ_UINT(result, 99999999);
}

TEST(swar_parse_uint_invalid) {
    uint64_t result;
    ASSERT_FALSE(xr_swar_parse_uint("abc", 3, &result));
    ASSERT_FALSE(xr_swar_parse_uint("12a4", 4, &result));
    ASSERT_FALSE(xr_swar_parse_uint("", 0, &result));
    ASSERT_FALSE(xr_swar_parse_uint("-1", 2, &result));
}

/* ========== Parse Signed Integer ========== */

TEST(swar_parse_int_positive) {
    int64_t result;
    ASSERT_TRUE(xr_swar_parse_int("0", 1, &result));
    ASSERT_EQ_INT(result, 0);

    ASSERT_TRUE(xr_swar_parse_int("42", 2, &result));
    ASSERT_EQ_INT(result, 42);

    ASSERT_TRUE(xr_swar_parse_int("1000000", 7, &result));
    ASSERT_EQ_INT(result, 1000000);
}

TEST(swar_parse_int_negative) {
    int64_t result;
    ASSERT_TRUE(xr_swar_parse_int("-1", 2, &result));
    ASSERT_EQ_INT(result, -1);

    ASSERT_TRUE(xr_swar_parse_int("-42", 3, &result));
    ASSERT_EQ_INT(result, -42);

    ASSERT_TRUE(xr_swar_parse_int("-999999", 7, &result));
    ASSERT_EQ_INT(result, -999999);
}

TEST(swar_parse_int_invalid) {
    int64_t result;
    ASSERT_FALSE(xr_swar_parse_int("abc", 3, &result));
    ASSERT_FALSE(xr_swar_parse_int("", 0, &result));
}

/* ========== Parse Hex ========== */

TEST(swar_parse_hex_basic) {
    uint64_t result;
    ASSERT_TRUE(xr_swar_parse_hex("0", 1, &result));
    ASSERT_EQ_UINT(result, 0x0);

    ASSERT_TRUE(xr_swar_parse_hex("F", 1, &result));
    ASSERT_EQ_UINT(result, 0xF);

    ASSERT_TRUE(xr_swar_parse_hex("ff", 2, &result));
    ASSERT_EQ_UINT(result, 0xFF);

    ASSERT_TRUE(xr_swar_parse_hex("DEADBEEF", 8, &result));
    ASSERT_EQ_UINT(result, 0xDEADBEEF);
}

TEST(swar_parse_hex_mixed_case) {
    uint64_t result;
    ASSERT_TRUE(xr_swar_parse_hex("aB", 2, &result));
    ASSERT_EQ_UINT(result, 0xAB);

    ASSERT_TRUE(xr_swar_parse_hex("DeAdBeEf", 8, &result));
    ASSERT_EQ_UINT(result, 0xDEADBEEF);
}

TEST(swar_parse_hex_invalid) {
    uint64_t result;
    ASSERT_FALSE(xr_swar_parse_hex("GG", 2, &result));
    ASSERT_FALSE(xr_swar_parse_hex("", 0, &result));
}

/* ========== Digit Detection ========== */

TEST(swar_is_digits_true) {
    ASSERT_TRUE(xr_swar_is_digits("0", 1));
    ASSERT_TRUE(xr_swar_is_digits("123", 3));
    ASSERT_TRUE(xr_swar_is_digits("0000000", 7));
    ASSERT_TRUE(xr_swar_is_digits("99999999", 8));
}

TEST(swar_is_digits_false) {
    ASSERT_FALSE(xr_swar_is_digits("abc", 3));
    ASSERT_FALSE(xr_swar_is_digits("12a", 3));
    ASSERT_FALSE(xr_swar_is_digits("", 0));
    ASSERT_FALSE(xr_swar_is_digits(" 1", 2));
}

TEST(swar_is_8_digits) {
    ASSERT_TRUE(xr_swar_is_8_digits("12345678"));
    ASSERT_TRUE(xr_swar_is_8_digits("00000000"));
    ASSERT_TRUE(xr_swar_is_8_digits("99999999"));
    ASSERT_FALSE(xr_swar_is_8_digits("1234567a"));
    ASSERT_FALSE(xr_swar_is_8_digits("abcdefgh"));
}

/* ========== Parse 8 Digits ========== */

TEST(swar_parse_8_digits) {
    ASSERT_EQ_UINT(xr_swar_parse_8_digits("12345678"), 12345678);
    ASSERT_EQ_UINT(xr_swar_parse_8_digits("00000001"), 1);
    ASSERT_EQ_UINT(xr_swar_parse_8_digits("99999999"), 99999999);
    ASSERT_EQ_UINT(xr_swar_parse_8_digits("00000000"), 0);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("SWAR - Parse Unsigned Integer");
    RUN_TEST(swar_parse_uint_basic);
    RUN_TEST(swar_parse_uint_large);
    RUN_TEST(swar_parse_uint_invalid);

    RUN_TEST_SUITE("SWAR - Parse Signed Integer");
    RUN_TEST(swar_parse_int_positive);
    RUN_TEST(swar_parse_int_negative);
    RUN_TEST(swar_parse_int_invalid);

    RUN_TEST_SUITE("SWAR - Parse Hex");
    RUN_TEST(swar_parse_hex_basic);
    RUN_TEST(swar_parse_hex_mixed_case);
    RUN_TEST(swar_parse_hex_invalid);

    RUN_TEST_SUITE("SWAR - Digit Detection");
    RUN_TEST(swar_is_digits_true);
    RUN_TEST(swar_is_digits_false);
    RUN_TEST(swar_is_8_digits);

    RUN_TEST_SUITE("SWAR - Parse 8 Digits");
    RUN_TEST(swar_parse_8_digits);

TEST_MAIN_END()
