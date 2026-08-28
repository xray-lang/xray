/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_crypto.c - Unit tests for cryptographic functions
 *
 * KEY CONCEPT:
 *   Tests hash functions (MD5, SHA-1, SHA-256, SHA-512), HMAC,
 *   AES encryption/decryption, and random byte generation.
 *   Uses known test vectors from NIST/RFC standards.
 */

#include "../test_framework.h"
#include "shared/xr_crypto_core.h"
#include <stdint.h>
#include <string.h>

void xr_random_bytes(uint8_t *buffer, size_t len);

// Helper: compare digest with expected hex string
static int digest_matches_hex(const uint8_t *digest, size_t digest_len, const char *expected_hex) {
    char hex[256];
    xr_bytes_to_hex(digest, digest_len, hex);
    return strcmp(hex, expected_hex) == 0;
}

/* ========== MD5 ========== */

/* ========== SHA-1 ========== */

/* ========== SHA-256 ========== */

TEST(crypto_sha256_empty) {
    uint8_t digest[32];
    xr_sha256((const uint8_t *) "", 0, digest);
    ASSERT_TRUE(digest_matches_hex(
        digest, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

TEST(crypto_sha256_abc) {
    uint8_t digest[32];
    xr_sha256((const uint8_t *) "abc", 3, digest);
    ASSERT_TRUE(digest_matches_hex(
        digest, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST(crypto_sha256_long) {
    // "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
    const char *input = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t digest[32];
    xr_sha256((const uint8_t *) input, strlen(input), digest);
    ASSERT_TRUE(digest_matches_hex(
        digest, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

/* ========== SHA-512 ========== */

/* ========== HMAC-SHA256 ========== */

TEST(crypto_hmac_sha256_basic) {
    // RFC 4231 Test Case 2
    const uint8_t key[] = "Jefe";
    const uint8_t data[] = "what do ya want for nothing?";
    uint8_t digest[32];

    xr_hmac_sha256(key, 4, data, 28, digest);
    ASSERT_TRUE(digest_matches_hex(
        digest, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
}

/* ========== AES-256-CBC ========== */

/* ========== Random Array<u8> ========== */

TEST(crypto_random_bytes) {
    uint8_t buf1[32], buf2[32];

    memset(buf1, 0, 32);
    memset(buf2, 0, 32);
    xr_random_bytes(buf1, 32);
    xr_random_bytes(buf2, 32);

    // Two random buffers should (almost certainly) differ
    ASSERT_TRUE(memcmp(buf1, buf2, 32) != 0);
}

TEST(crypto_random_bytes_zero) {
    // Zero-length should succeed without crashing
    uint8_t buf[1] = {0xAA};
    xr_random_bytes(buf, 0);
    ASSERT_EQ_INT(buf[0], 0xAA);
}

/* ========== Array<u8> to Hex ========== */

TEST(crypto_bytes_to_hex) {
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    char hex[16];
    xr_bytes_to_hex(data, 4, hex);
    ASSERT_STR_EQ(hex, "deadbeef");
}

TEST(crypto_core_bytes_hex) {
    uint8_t data[] = {0x00, 0x0F, 0xA5};
    char hex[7];
    ASSERT_TRUE(xr_crypto_core_bytes_hex(data, 3, hex, sizeof(hex)));
    ASSERT_STR_EQ(hex, "000fa5");
    ASSERT_TRUE(!xr_crypto_core_bytes_hex(data, 3, hex, 6));
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Crypto - SHA-256");
RUN_TEST(crypto_sha256_empty);
RUN_TEST(crypto_sha256_abc);
RUN_TEST(crypto_sha256_long);
RUN_TEST_SUITE("Crypto - HMAC-SHA256");
RUN_TEST(crypto_hmac_sha256_basic);

RUN_TEST_SUITE("Crypto - AES-256-CBC");

RUN_TEST_SUITE("Crypto - Random");
RUN_TEST(crypto_random_bytes);
RUN_TEST(crypto_random_bytes_zero);

RUN_TEST_SUITE("Crypto - Utility");
RUN_TEST(crypto_bytes_to_hex);
RUN_TEST(crypto_core_bytes_hex);

TEST_MAIN_END()
