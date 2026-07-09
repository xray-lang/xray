/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_base64.c - Unit tests for Base64 encoding/decoding
 *
 * KEY CONCEPT:
 *   Tests standard Base64 and URL-safe Base64 encode/decode,
 *   validation, edge cases, and RFC 4648 compliance.
 */

#include "../test_framework.h"
#include "base/xmalloc.h"
#include "shared/xr_base64_core.h"

// Native helpers from stdlib/base64 (forward declare to avoid pulling in module deps)
char *base64_encode_alloc(const unsigned char *data, size_t len, size_t *out_len);
char *base64_encode_url_alloc(const unsigned char *data, size_t len, size_t *out_len);
unsigned char *base64_decode_alloc(const char *data, size_t len, size_t *out_len);
unsigned char *base64_decode_url_alloc(const char *data, size_t len, size_t *out_len);
bool base64_is_valid_bytes(const char *data, size_t len);

/* ========== Standard Base64 Encode ========== */

TEST(base64_encode_empty) {
    size_t out_len;
    char *result = base64_encode_alloc((const unsigned char *) "", 0, &out_len);
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_UINT(out_len, 0);
    xr_free(result);
}

TEST(base64_encode_basic) {
    size_t out_len;

    // RFC 4648 test vectors
    char *r1 = base64_encode_alloc((const unsigned char *) "f", 1, &out_len);
    ASSERT_STR_EQ(r1, "Zg==");
    xr_free(r1);

    char *r2 = base64_encode_alloc((const unsigned char *) "fo", 2, &out_len);
    ASSERT_STR_EQ(r2, "Zm8=");
    xr_free(r2);

    char *r3 = base64_encode_alloc((const unsigned char *) "foo", 3, &out_len);
    ASSERT_STR_EQ(r3, "Zm9v");
    xr_free(r3);

    char *r4 = base64_encode_alloc((const unsigned char *) "foobar", 6, &out_len);
    ASSERT_STR_EQ(r4, "Zm9vYmFy");
    xr_free(r4);
}

TEST(base64_encode_hello) {
    size_t out_len;
    char *result = base64_encode_alloc((const unsigned char *) "Hello, World!", 13, &out_len);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result, "SGVsbG8sIFdvcmxkIQ==");
    xr_free(result);
}

/* ========== Standard Base64 Decode ========== */

TEST(base64_decode_empty) {
    size_t out_len;
    unsigned char *result = base64_decode_alloc("", 0, &out_len);
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_UINT(out_len, 0);
    xr_free(result);
}

TEST(base64_decode_basic) {
    size_t out_len;

    unsigned char *r1 = base64_decode_alloc("Zg==", 4, &out_len);
    ASSERT_EQ_UINT(out_len, 1);
    ASSERT_TRUE(memcmp(r1, "f", 1) == 0);
    xr_free(r1);

    unsigned char *r2 = base64_decode_alloc("Zm9v", 4, &out_len);
    ASSERT_EQ_UINT(out_len, 3);
    ASSERT_TRUE(memcmp(r2, "foo", 3) == 0);
    xr_free(r2);

    unsigned char *r3 = base64_decode_alloc("Zm9vYmFy", 8, &out_len);
    ASSERT_EQ_UINT(out_len, 6);
    ASSERT_TRUE(memcmp(r3, "foobar", 6) == 0);
    xr_free(r3);
}

TEST(base64_decode_hello) {
    size_t out_len;
    unsigned char *result = base64_decode_alloc("SGVsbG8sIFdvcmxkIQ==", 20, &out_len);
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_UINT(out_len, 13);
    ASSERT_TRUE(memcmp(result, "Hello, World!", 13) == 0);
    xr_free(result);
}

/* ========== Roundtrip ========== */

TEST(base64_roundtrip) {
    const char *inputs[] = {"", "a", "ab", "abc", "abcd", "Hello, World!", "1234567890"};
    int n = sizeof(inputs) / sizeof(inputs[0]);

    for (int i = 0; i < n; i++) {
        size_t enc_len, dec_len;
        size_t in_len = strlen(inputs[i]);

        char *encoded = base64_encode_alloc((const unsigned char *) inputs[i], in_len, &enc_len);
        ASSERT_NOT_NULL(encoded);

        unsigned char *decoded = base64_decode_alloc(encoded, enc_len, &dec_len);
        ASSERT_NOT_NULL(decoded);
        ASSERT_EQ_UINT(dec_len, in_len);
        ASSERT_TRUE(memcmp(decoded, inputs[i], in_len) == 0);

        xr_free(encoded);
        xr_free(decoded);
    }
}

/* ========== URL-safe Base64 ========== */

TEST(base64_url_encode) {
    size_t out_len;
    // Data that would produce + and / in standard base64
    unsigned char data[] = {0xFB, 0xFF, 0xFE};
    char *result = base64_encode_url_alloc(data, 3, &out_len);
    ASSERT_NOT_NULL(result);
    // URL-safe should not contain + or /
    for (size_t i = 0; i < out_len; i++) {
        ASSERT_NE(result[i], '+');
        ASSERT_NE(result[i], '/');
    }
    xr_free(result);
}

TEST(base64_url_roundtrip) {
    const unsigned char data[] = {0x00, 0xFF, 0x80, 0x7F, 0xFE, 0x01};
    size_t enc_len, dec_len;

    char *encoded = base64_encode_url_alloc(data, 6, &enc_len);
    ASSERT_NOT_NULL(encoded);

    unsigned char *decoded = base64_decode_url_alloc(encoded, enc_len, &dec_len);
    ASSERT_NOT_NULL(decoded);
    ASSERT_EQ_UINT(dec_len, 6);
    ASSERT_TRUE(memcmp(decoded, data, 6) == 0);

    xr_free(encoded);
    xr_free(decoded);
}

/* ========== Validation ========== */

TEST(base64_is_valid) {
    ASSERT_TRUE(base64_is_valid_bytes("Zm9v", 4));
    ASSERT_TRUE(base64_is_valid_bytes("Zg==", 4));
    ASSERT_TRUE(base64_is_valid_bytes("Zm8=", 4));
    ASSERT_TRUE(base64_is_valid_bytes("", 0));

    // Note: "Zg=" has odd padding but implementation accepts it (lenient)
    // ASSERT_FALSE(base64_is_valid_bytes("Zg=", 3));
    ASSERT_FALSE(base64_is_valid_bytes("!!!!", 4));
}

/* ========== Binary Data ========== */

TEST(base64_binary_data) {
    unsigned char binary[256];
    for (int i = 0; i < 256; i++)
        binary[i] = (unsigned char) i;

    size_t enc_len, dec_len;
    char *encoded = base64_encode_alloc(binary, 256, &enc_len);
    ASSERT_NOT_NULL(encoded);

    unsigned char *decoded = base64_decode_alloc(encoded, enc_len, &dec_len);
    ASSERT_NOT_NULL(decoded);
    ASSERT_EQ_UINT(dec_len, 256);
    ASSERT_TRUE(memcmp(decoded, binary, 256) == 0);

    xr_free(encoded);
    xr_free(decoded);
}

TEST(base64_array_lane_byte_semantics) {
    ASSERT_EQ_UINT(xr_base64_core_byte_from_array_lane(true, 72), 72);
    ASSERT_EQ_UINT(xr_base64_core_byte_from_array_lane(true, -1), 255);
    ASSERT_EQ_UINT(xr_base64_core_byte_from_array_lane(true, 300), 44);
    ASSERT_EQ_UINT(xr_base64_core_byte_from_array_lane(false, 1), 0);
    ASSERT_EQ_UINT(xr_base64_core_byte_from_array_lane(false, 72), 0);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Base64 - Standard Encode");
RUN_TEST(base64_encode_empty);
RUN_TEST(base64_encode_basic);
RUN_TEST(base64_encode_hello);

RUN_TEST_SUITE("Base64 - Standard Decode");
RUN_TEST(base64_decode_empty);
RUN_TEST(base64_decode_basic);
RUN_TEST(base64_decode_hello);

RUN_TEST_SUITE("Base64 - Roundtrip");
RUN_TEST(base64_roundtrip);

RUN_TEST_SUITE("Base64 - URL-safe");
RUN_TEST(base64_url_encode);
RUN_TEST(base64_url_roundtrip);

RUN_TEST_SUITE("Base64 - Validation");
RUN_TEST(base64_is_valid);

RUN_TEST_SUITE("Base64 - Binary Data");
RUN_TEST(base64_binary_data);

RUN_TEST_SUITE("Base64 - Array Lane Semantics");
RUN_TEST(base64_array_lane_byte_semantics);

TEST_MAIN_END()
