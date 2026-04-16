/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_utf8.c - Unit tests for UTF-8 encoding/decoding
 */

#include "../test_framework.h"
#include "runtime/object/xutf8.h"
#include <string.h>

/* ========== Decode Tests ========== */

TEST(utf8_decode_ascii) {
    uint32_t cp;
    int bytes = xr_utf8_decode("A", 1, &cp);
    ASSERT_EQ_INT(bytes, 1);
    ASSERT_EQ_INT(cp, 'A');
    
    bytes = xr_utf8_decode("z", 1, &cp);
    ASSERT_EQ_INT(bytes, 1);
    ASSERT_EQ_INT(cp, 'z');
    
    bytes = xr_utf8_decode("0", 1, &cp);
    ASSERT_EQ_INT(bytes, 1);
    ASSERT_EQ_INT(cp, '0');
}

TEST(utf8_decode_2byte) {
    uint32_t cp;
    // é = U+00E9 = 0xC3 0xA9
    const char *e_acute = "\xC3\xA9";
    int bytes = xr_utf8_decode(e_acute, 2, &cp);
    ASSERT_EQ_INT(bytes, 2);
    ASSERT_EQ_INT(cp, 0x00E9);
    
    // ñ = U+00F1 = 0xC3 0xB1
    const char *n_tilde = "\xC3\xB1";
    bytes = xr_utf8_decode(n_tilde, 2, &cp);
    ASSERT_EQ_INT(bytes, 2);
    ASSERT_EQ_INT(cp, 0x00F1);
}

TEST(utf8_decode_3byte) {
    uint32_t cp;
    // 中 = U+4E2D = 0xE4 0xB8 0xAD
    const char *zhong = "\xE4\xB8\xAD";
    int bytes = xr_utf8_decode(zhong, 3, &cp);
    ASSERT_EQ_INT(bytes, 3);
    ASSERT_EQ_INT(cp, 0x4E2D);
    
    // € = U+20AC = 0xE2 0x82 0xAC
    const char *euro = "\xE2\x82\xAC";
    bytes = xr_utf8_decode(euro, 3, &cp);
    ASSERT_EQ_INT(bytes, 3);
    ASSERT_EQ_INT(cp, 0x20AC);
}

TEST(utf8_decode_4byte) {
    uint32_t cp;
    // 😀 = U+1F600 = 0xF0 0x9F 0x98 0x80
    const char *emoji = "\xF0\x9F\x98\x80";
    int bytes = xr_utf8_decode(emoji, 4, &cp);
    ASSERT_EQ_INT(bytes, 4);
    ASSERT_EQ_INT(cp, 0x1F600);
    
    // 𝄞 = U+1D11E = 0xF0 0x9D 0x84 0x9E
    const char *clef = "\xF0\x9D\x84\x9E";
    bytes = xr_utf8_decode(clef, 4, &cp);
    ASSERT_EQ_INT(bytes, 4);
    ASSERT_EQ_INT(cp, 0x1D11E);
}

/* ========== Encode Tests ========== */

TEST(utf8_encode_ascii) {
    char buf[5] = {0};
    int bytes = xr_utf8_encode('A', buf);
    ASSERT_EQ_INT(bytes, 1);
    ASSERT_EQ_INT(buf[0], 'A');
    
    bytes = xr_utf8_encode('z', buf);
    ASSERT_EQ_INT(bytes, 1);
    ASSERT_EQ_INT(buf[0], 'z');
}

TEST(utf8_encode_2byte) {
    char buf[5] = {0};
    // é = U+00E9
    int bytes = xr_utf8_encode(0x00E9, buf);
    ASSERT_EQ_INT(bytes, 2);
    ASSERT_EQ_INT((unsigned char)buf[0], 0xC3);
    ASSERT_EQ_INT((unsigned char)buf[1], 0xA9);
}

TEST(utf8_encode_3byte) {
    char buf[5] = {0};
    // 中 = U+4E2D
    int bytes = xr_utf8_encode(0x4E2D, buf);
    ASSERT_EQ_INT(bytes, 3);
    ASSERT_EQ_INT((unsigned char)buf[0], 0xE4);
    ASSERT_EQ_INT((unsigned char)buf[1], 0xB8);
    ASSERT_EQ_INT((unsigned char)buf[2], 0xAD);
}

TEST(utf8_encode_4byte) {
    char buf[5] = {0};
    // 😀 = U+1F600
    int bytes = xr_utf8_encode(0x1F600, buf);
    ASSERT_EQ_INT(bytes, 4);
    ASSERT_EQ_INT((unsigned char)buf[0], 0xF0);
    ASSERT_EQ_INT((unsigned char)buf[1], 0x9F);
    ASSERT_EQ_INT((unsigned char)buf[2], 0x98);
    ASSERT_EQ_INT((unsigned char)buf[3], 0x80);
}

/* ========== Roundtrip Tests ========== */

TEST(utf8_roundtrip) {
    uint32_t test_cps[] = {
        'A', 0x00E9, 0x4E2D, 0x1F600, 0x10FFFF
    };
    
    for (int i = 0; i < 5; i++) {
        char buf[5] = {0};
        int enc_bytes = xr_utf8_encode(test_cps[i], buf);
        ASSERT_TRUE(enc_bytes > 0);
        
        uint32_t decoded;
        int dec_bytes = xr_utf8_decode(buf, enc_bytes, &decoded);
        ASSERT_EQ_INT(enc_bytes, dec_bytes);
        ASSERT_EQ_INT(decoded, test_cps[i]);
    }
}

/* ========== String Length Tests ========== */

TEST(utf8_strlen_ascii) {
    const char *s = "Hello";
    size_t len = xr_utf8_strlen(s, strlen(s));
    ASSERT_EQ_INT(len, 5);
}

TEST(utf8_strlen_mixed) {
    // "Hello中文" = 5 ASCII + 2 Chinese = 7 chars
    const char *s = "Hello\xE4\xB8\xAD\xE6\x96\x87";
    size_t len = xr_utf8_strlen(s, strlen(s));
    ASSERT_EQ_INT(len, 7);
}

TEST(utf8_strlen_emoji) {
    // "Hi😀" = 2 ASCII + 1 emoji = 3 chars
    const char *s = "Hi\xF0\x9F\x98\x80";
    size_t len = xr_utf8_strlen(s, strlen(s));
    ASSERT_EQ_INT(len, 3);
}

TEST(utf8_strlen_empty) {
    size_t len = xr_utf8_strlen("", 0);
    ASSERT_EQ_INT(len, 0);
}

/* ========== Index Conversion Tests ========== */

TEST(utf8_index_to_offset_ascii) {
    const char *s = "Hello";
    size_t len = strlen(s);
    
    ASSERT_EQ_INT(xr_utf8_index_to_offset(s, len, 0), 0);
    ASSERT_EQ_INT(xr_utf8_index_to_offset(s, len, 2), 2);
    ASSERT_EQ_INT(xr_utf8_index_to_offset(s, len, 5), 5);
}

TEST(utf8_index_to_offset_multibyte) {
    // "中文Hi" = 中(3) + 文(3) + H(1) + i(1) = 8 bytes
    const char *s = "\xE4\xB8\xAD\xE6\x96\x87Hi";
    size_t len = strlen(s);
    
    ASSERT_EQ_INT(xr_utf8_index_to_offset(s, len, 0), 0);  // 中
    ASSERT_EQ_INT(xr_utf8_index_to_offset(s, len, 1), 3);  // 文
    ASSERT_EQ_INT(xr_utf8_index_to_offset(s, len, 2), 6);  // H
    ASSERT_EQ_INT(xr_utf8_index_to_offset(s, len, 3), 7);  // i
}

TEST(utf8_offset_to_index_ascii) {
    const char *s = "Hello";
    size_t len = strlen(s);
    
    ASSERT_EQ_INT(xr_utf8_offset_to_index(s, len, 0), 0);
    ASSERT_EQ_INT(xr_utf8_offset_to_index(s, len, 2), 2);
    ASSERT_EQ_INT(xr_utf8_offset_to_index(s, len, 5), 5);
}

TEST(utf8_offset_to_index_multibyte) {
    // "中文Hi"
    const char *s = "\xE4\xB8\xAD\xE6\x96\x87Hi";
    size_t len = strlen(s);
    
    ASSERT_EQ_INT(xr_utf8_offset_to_index(s, len, 0), 0);  // start of 中
    ASSERT_EQ_INT(xr_utf8_offset_to_index(s, len, 3), 1);  // start of 文
    ASSERT_EQ_INT(xr_utf8_offset_to_index(s, len, 6), 2);  // start of H
    ASSERT_EQ_INT(xr_utf8_offset_to_index(s, len, 7), 3);  // start of i
}

/* ========== Char At Tests ========== */

TEST(utf8_char_at) {
    // "A中B"
    const char *s = "A\xE4\xB8\xAD" "B";
    size_t len = strlen(s);
    uint32_t cp;
    size_t pos;
    
    ASSERT_TRUE(xr_utf8_char_at(s, len, 0, &cp, &pos));
    ASSERT_EQ_INT(cp, 'A');
    ASSERT_EQ_INT(pos, 0);
    
    ASSERT_TRUE(xr_utf8_char_at(s, len, 1, &cp, &pos));
    ASSERT_EQ_INT(cp, 0x4E2D);  // 中
    ASSERT_EQ_INT(pos, 1);
    
    ASSERT_TRUE(xr_utf8_char_at(s, len, 2, &cp, &pos));
    ASSERT_EQ_INT(cp, 'B');
    ASSERT_EQ_INT(pos, 4);
}

/* ========== Validation Tests ========== */

TEST(utf8_validate_valid) {
    ASSERT_TRUE(xr_utf8_validate("Hello", 5));
    ASSERT_TRUE(xr_utf8_validate("\xE4\xB8\xAD", 3));  // 中
    ASSERT_TRUE(xr_utf8_validate("\xF0\x9F\x98\x80", 4));  // 😀
    ASSERT_TRUE(xr_utf8_validate("", 0));
}

TEST(utf8_validate_invalid) {
    // Invalid continuation byte
    ASSERT_FALSE(xr_utf8_validate("\x80", 1));
    // Incomplete sequence
    ASSERT_FALSE(xr_utf8_validate("\xC3", 1));
    // Overlong encoding
    ASSERT_FALSE(xr_utf8_validate("\xC0\x80", 2));
}

/* ========== Char Size Tests ========== */

TEST(utf8_char_size) {
    ASSERT_EQ_INT(xr_utf8_char_size(0x00), 1);  // ASCII
    ASSERT_EQ_INT(xr_utf8_char_size(0x7F), 1);  // ASCII
    ASSERT_EQ_INT(xr_utf8_char_size(0xC3), 2);  // 2-byte start
    ASSERT_EQ_INT(xr_utf8_char_size(0xE4), 3);  // 3-byte start
    ASSERT_EQ_INT(xr_utf8_char_size(0xF0), 4);  // 4-byte start
}

TEST(utf8_encode_size) {
    ASSERT_EQ_INT(xr_utf8_encode_size(0x00), 1);
    ASSERT_EQ_INT(xr_utf8_encode_size(0x7F), 1);
    ASSERT_EQ_INT(xr_utf8_encode_size(0x80), 2);
    ASSERT_EQ_INT(xr_utf8_encode_size(0x7FF), 2);
    ASSERT_EQ_INT(xr_utf8_encode_size(0x800), 3);
    ASSERT_EQ_INT(xr_utf8_encode_size(0xFFFF), 3);
    ASSERT_EQ_INT(xr_utf8_encode_size(0x10000), 4);
    ASSERT_EQ_INT(xr_utf8_encode_size(0x10FFFF), 4);
    ASSERT_EQ_INT(xr_utf8_encode_size(0x110000), 0);  // invalid
}

TEST(utf8_is_continuation) {
    ASSERT_TRUE(xr_utf8_is_continuation(0x80));
    ASSERT_TRUE(xr_utf8_is_continuation(0xBF));
    ASSERT_FALSE(xr_utf8_is_continuation(0x00));
    ASSERT_FALSE(xr_utf8_is_continuation(0x7F));
    ASSERT_FALSE(xr_utf8_is_continuation(0xC0));
}

/* ========== Main ========== */

static void run_all_tests(void) {
    RUN_TEST_SUITE("Decode");
    RUN_TEST(utf8_decode_ascii);
    RUN_TEST(utf8_decode_2byte);
    RUN_TEST(utf8_decode_3byte);
    RUN_TEST(utf8_decode_4byte);
    
    RUN_TEST_SUITE("Encode");
    RUN_TEST(utf8_encode_ascii);
    RUN_TEST(utf8_encode_2byte);
    RUN_TEST(utf8_encode_3byte);
    RUN_TEST(utf8_encode_4byte);
    
    RUN_TEST_SUITE("Roundtrip");
    RUN_TEST(utf8_roundtrip);
    
    RUN_TEST_SUITE("String Length");
    RUN_TEST(utf8_strlen_ascii);
    RUN_TEST(utf8_strlen_mixed);
    RUN_TEST(utf8_strlen_emoji);
    RUN_TEST(utf8_strlen_empty);
    
    RUN_TEST_SUITE("Index Conversion");
    RUN_TEST(utf8_index_to_offset_ascii);
    RUN_TEST(utf8_index_to_offset_multibyte);
    RUN_TEST(utf8_offset_to_index_ascii);
    RUN_TEST(utf8_offset_to_index_multibyte);
    
    RUN_TEST_SUITE("Char At");
    RUN_TEST(utf8_char_at);
    
    RUN_TEST_SUITE("Validation");
    RUN_TEST(utf8_validate_valid);
    RUN_TEST(utf8_validate_invalid);
    
    RUN_TEST_SUITE("Size Functions");
    RUN_TEST(utf8_char_size);
    RUN_TEST(utf8_encode_size);
    RUN_TEST(utf8_is_continuation);
}

TEST_MAIN_BEGIN()
    printf("=== xray UTF-8 Unit Tests ===\n");
    run_all_tests();
TEST_MAIN_END()
