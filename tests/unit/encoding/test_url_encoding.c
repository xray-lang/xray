/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_url_encoding.c - Unit tests for URL encoding/decoding
 *
 * KEY CONCEPT:
 *   Tests RFC 3986 percent-encoding, form encoding, and roundtrip behavior.
 */

#include "../test_framework.h"

// C-level API from stdlib
int xr_url_encode(const char *str, size_t len, char *buf, size_t buf_size);
int xr_url_decode(const char *str, size_t len, char *buf, size_t buf_size);
int xr_url_encode_form(const char *str, size_t len, char *buf, size_t buf_size);
int xr_url_decode_form(const char *str, size_t len, char *buf, size_t buf_size);

/* ========== RFC 3986 Encode ========== */

TEST(url_encode_passthrough) {
    char buf[256];
    // Unreserved chars should pass through: A-Z a-z 0-9 - _ . ~
    int n = xr_url_encode("Hello-World_2026.v5~", 20, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    ASSERT_STR_EQ(buf, "Hello-World_2026.v5~");
}

TEST(url_encode_special) {
    char buf[256];
    int n = xr_url_encode("hello world!", 12, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    // space -> %20, ! -> %21
    ASSERT_TRUE(strstr(buf, "%20") != NULL);
    ASSERT_TRUE(strstr(buf, "%21") != NULL);
}

TEST(url_encode_unicode) {
    char buf[256];
    // UTF-8 bytes for a multi-byte char should each be percent-encoded
    int n = xr_url_encode("a/b", 3, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    ASSERT_TRUE(strstr(buf, "%2F") != NULL || strstr(buf, "%2f") != NULL);
}

TEST(url_encode_empty) {
    char buf[64];
    int n = xr_url_encode("", 0, buf, sizeof(buf));
    ASSERT_EQ_INT(n, 0);
    ASSERT_STR_EQ(buf, "");
}

/* ========== RFC 3986 Decode ========== */

TEST(url_decode_basic) {
    char buf[256];
    int n = xr_url_decode("Hello%20World%21", 16, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    ASSERT_STR_EQ(buf, "Hello World!");
}

TEST(url_decode_passthrough) {
    char buf[256];
    int n = xr_url_decode("abc123", 6, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    ASSERT_STR_EQ(buf, "abc123");
}

TEST(url_decode_empty) {
    char buf[64];
    int n = xr_url_decode("", 0, buf, sizeof(buf));
    ASSERT_EQ_INT(n, 0);
}

TEST(url_decode_plus_not_space) {
    char buf[256];
    // RFC 3986: + is NOT treated as space
    int n = xr_url_decode("a+b", 3, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    ASSERT_STR_EQ(buf, "a+b");
}

/* ========== Form Encode/Decode ========== */

TEST(url_form_encode_space) {
    char buf[256];
    int n = xr_url_encode_form("hello world", 11, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    // Form encoding: space -> +
    ASSERT_TRUE(strstr(buf, "+") != NULL);
    ASSERT_TRUE(strstr(buf, "%20") == NULL);
}

TEST(url_form_decode_plus) {
    char buf[256];
    int n = xr_url_decode_form("hello+world", 11, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    ASSERT_STR_EQ(buf, "hello world");
}

TEST(url_form_decode_percent) {
    char buf[256];
    int n = xr_url_decode_form("a%26b%3Dc", 9, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    ASSERT_STR_EQ(buf, "a&b=c");
}

/* ========== Roundtrip ========== */

TEST(url_roundtrip) {
    const char *inputs[] = {"hello", "a b c", "key=value&foo=bar", "100%", "/path/to/file"};
    int n = sizeof(inputs) / sizeof(inputs[0]);

    for (int i = 0; i < n; i++) {
        char enc[512], dec[512];
        size_t in_len = strlen(inputs[i]);

        int enc_len = xr_url_encode(inputs[i], in_len, enc, sizeof(enc));
        ASSERT_GT(enc_len, 0);

        int dec_len = xr_url_decode(enc, (size_t)enc_len, dec, sizeof(dec));
        ASSERT_GT(dec_len, 0);
        ASSERT_STR_EQ(dec, inputs[i]);
    }
}

TEST(url_form_roundtrip) {
    const char *inputs[] = {"hello world", "a+b=c&d", "spaces  here"};
    int n = sizeof(inputs) / sizeof(inputs[0]);

    for (int i = 0; i < n; i++) {
        char enc[512], dec[512];
        size_t in_len = strlen(inputs[i]);

        int enc_len = xr_url_encode_form(inputs[i], in_len, enc, sizeof(enc));
        ASSERT_GT(enc_len, 0);

        int dec_len = xr_url_decode_form(enc, (size_t)enc_len, dec, sizeof(dec));
        ASSERT_GT(dec_len, 0);
        ASSERT_STR_EQ(dec, inputs[i]);
    }
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("URL - RFC 3986 Encode");
    RUN_TEST(url_encode_passthrough);
    RUN_TEST(url_encode_special);
    RUN_TEST(url_encode_unicode);
    RUN_TEST(url_encode_empty);

    RUN_TEST_SUITE("URL - RFC 3986 Decode");
    RUN_TEST(url_decode_basic);
    RUN_TEST(url_decode_passthrough);
    RUN_TEST(url_decode_empty);
    RUN_TEST(url_decode_plus_not_space);

    RUN_TEST_SUITE("URL - Form Encode/Decode");
    RUN_TEST(url_form_encode_space);
    RUN_TEST(url_form_decode_plus);
    RUN_TEST(url_form_decode_percent);

    RUN_TEST_SUITE("URL - Roundtrip");
    RUN_TEST(url_roundtrip);
    RUN_TEST(url_form_roundtrip);

TEST_MAIN_END()
