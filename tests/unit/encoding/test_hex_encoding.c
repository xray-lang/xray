/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_hex_encoding.c - Unit tests for hex encoding/decoding
 *
 * KEY CONCEPT:
 *   Tests hex encode, decode, validation, and roundtrip for binary data.
 */

#include "../test_framework.h"
#include <stdbool.h>
#include <stdint.h>

// C-level API from stdlib/encoding
int xr_hex_encode(const uint8_t *data, size_t len, char *output);
int xr_hex_decode(const char *hex, size_t len, uint8_t *output);
bool xr_hex_valid(const char *hex, size_t len);

/* ========== Hex Encode ========== */

TEST(hex_encode_empty) {
    char buf[4];
    int n = xr_hex_encode((const uint8_t*)"", 0, buf);
    ASSERT_EQ_INT(n, 0);
}

TEST(hex_encode_basic) {
    char buf[64];
    uint8_t data[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello"
    int n = xr_hex_encode(data, 5, buf);
    ASSERT_EQ_INT(n, 10);
    buf[n] = '\0';
    // Should produce lowercase hex
    ASSERT_TRUE(strcmp(buf, "48656c6c6f") == 0 || strcmp(buf, "48656C6C6F") == 0);
}

TEST(hex_encode_all_bytes) {
    char buf[8];
    uint8_t data[] = {0x00, 0xFF, 0xAB};
    int n = xr_hex_encode(data, 3, buf);
    ASSERT_EQ_INT(n, 6);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "00") == buf);  // starts with 00
}

/* ========== Hex Decode ========== */

TEST(hex_decode_basic) {
    uint8_t buf[64];
    int n = xr_hex_decode("48656c6c6f", 10, buf);
    ASSERT_EQ_INT(n, 5);
    ASSERT_TRUE(memcmp(buf, "Hello", 5) == 0);
}

TEST(hex_decode_uppercase) {
    uint8_t buf[64];
    int n = xr_hex_decode("48656C6C6F", 10, buf);
    ASSERT_EQ_INT(n, 5);
    ASSERT_TRUE(memcmp(buf, "Hello", 5) == 0);
}

TEST(hex_decode_empty) {
    uint8_t buf[4];
    int n = xr_hex_decode("", 0, buf);
    ASSERT_EQ_INT(n, 0);
}

/* ========== Hex Validation ========== */

TEST(hex_valid_true) {
    ASSERT_TRUE(xr_hex_valid("0123456789abcdef", 16));
    ASSERT_TRUE(xr_hex_valid("ABCDEF", 6));
    ASSERT_TRUE(xr_hex_valid("", 0));
    ASSERT_TRUE(xr_hex_valid("00ff", 4));
}

TEST(hex_valid_false) {
    ASSERT_FALSE(xr_hex_valid("0g", 2));       // 'g' invalid
    ASSERT_FALSE(xr_hex_valid("xyz", 3));
    ASSERT_FALSE(xr_hex_valid("abc", 3));       // odd length
}

/* ========== Roundtrip ========== */

TEST(hex_roundtrip) {
    uint8_t original[256];
    for (int i = 0; i < 256; i++) original[i] = (uint8_t)i;

    char encoded[514];
    int enc_len = xr_hex_encode(original, 256, encoded);
    ASSERT_EQ_INT(enc_len, 512);
    encoded[enc_len] = '\0';

    uint8_t decoded[256];
    int dec_len = xr_hex_decode(encoded, (size_t)enc_len, decoded);
    ASSERT_EQ_INT(dec_len, 256);
    ASSERT_TRUE(memcmp(decoded, original, 256) == 0);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("Hex - Encode");
    RUN_TEST(hex_encode_empty);
    RUN_TEST(hex_encode_basic);
    RUN_TEST(hex_encode_all_bytes);

    RUN_TEST_SUITE("Hex - Decode");
    RUN_TEST(hex_decode_basic);
    RUN_TEST(hex_decode_uppercase);
    RUN_TEST(hex_decode_empty);

    RUN_TEST_SUITE("Hex - Validation");
    RUN_TEST(hex_valid_true);
    RUN_TEST(hex_valid_false);

    RUN_TEST_SUITE("Hex - Roundtrip");
    RUN_TEST(hex_roundtrip);

TEST_MAIN_END()
