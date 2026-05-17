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
#include <stdint.h>
#include <string.h>

// Forward declare C-level API from stdlib/crypto
typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t buffer[64];
} XrMD5Context;
typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} XrSHA1Context;
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} XrSHA256Context;
typedef struct {
    uint64_t state[8];
    uint64_t count[2];
    uint8_t buffer[128];
} XrSHA512Context;
typedef struct {
    uint32_t round_key[60];
    int rounds;
} XrAESContext;

void xr_md5(const uint8_t *data, size_t len, uint8_t digest[16]);
void xr_sha1(const uint8_t *data, size_t len, uint8_t digest[20]);
void xr_sha256(const uint8_t *data, size_t len, uint8_t digest[32]);
void xr_sha512(const uint8_t *data, size_t len, uint8_t digest[64]);

void xr_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                    uint8_t digest[32]);

void xr_aes_init(XrAESContext *ctx, const uint8_t *key, int key_bits);
void xr_aes_cbc_encrypt(XrAESContext *ctx, const uint8_t *iv, const uint8_t *input, uint8_t *output,
                        size_t len);
void xr_aes_cbc_decrypt(XrAESContext *ctx, const uint8_t *iv, const uint8_t *input, uint8_t *output,
                        size_t len);

void xr_random_bytes(uint8_t *buffer, size_t len);
void xr_bytes_to_hex(const uint8_t *bytes, size_t len, char *output);

// Helper: compare digest with expected hex string
static int digest_matches_hex(const uint8_t *digest, size_t digest_len, const char *expected_hex) {
    char hex[256];
    xr_bytes_to_hex(digest, digest_len, hex);
    return strcmp(hex, expected_hex) == 0;
}

/* ========== MD5 ========== */

TEST(crypto_md5_empty) {
    uint8_t digest[16];
    xr_md5((const uint8_t *) "", 0, digest);
    ASSERT_TRUE(digest_matches_hex(digest, 16, "d41d8cd98f00b204e9800998ecf8427e"));
}

TEST(crypto_md5_abc) {
    uint8_t digest[16];
    xr_md5((const uint8_t *) "abc", 3, digest);
    ASSERT_TRUE(digest_matches_hex(digest, 16, "900150983cd24fb0d6963f7d28e17f72"));
}

TEST(crypto_md5_hello) {
    uint8_t digest[16];
    xr_md5((const uint8_t *) "Hello, World!", 13, digest);
    ASSERT_TRUE(digest_matches_hex(digest, 16, "65a8e27d8879283831b664bd8b7f0ad4"));
}

/* ========== SHA-1 ========== */

TEST(crypto_sha1_empty) {
    uint8_t digest[20];
    xr_sha1((const uint8_t *) "", 0, digest);
    ASSERT_TRUE(digest_matches_hex(digest, 20, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
}

TEST(crypto_sha1_abc) {
    uint8_t digest[20];
    xr_sha1((const uint8_t *) "abc", 3, digest);
    ASSERT_TRUE(digest_matches_hex(digest, 20, "a9993e364706816aba3e25717850c26c9cd0d89d"));
}

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

TEST(crypto_sha512_empty) {
    uint8_t digest[64];
    xr_sha512((const uint8_t *) "", 0, digest);
    ASSERT_TRUE(
        digest_matches_hex(digest, 64,
                           "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                           "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));
}

TEST(crypto_sha512_abc) {
    uint8_t digest[64];
    xr_sha512((const uint8_t *) "abc", 3, digest);
    ASSERT_TRUE(
        digest_matches_hex(digest, 64,
                           "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                           "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
}

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

TEST(crypto_aes_cbc_roundtrip) {
    // 256-bit key (32 bytes)
    uint8_t key[32];
    memset(key, 0x42, 32);

    // 16-byte IV
    uint8_t iv[16];
    memset(iv, 0x01, 16);

    // 32 bytes plaintext (2 blocks)
    uint8_t plaintext[32] = "Hello AES-256 CBC encryption!!\0";
    uint8_t ciphertext[32];
    uint8_t decrypted[32];

    XrAESContext ctx;
    xr_aes_init(&ctx, key, 256);

    xr_aes_cbc_encrypt(&ctx, iv, plaintext, ciphertext, 32);

    // Ciphertext should differ from plaintext
    ASSERT_TRUE(memcmp(plaintext, ciphertext, 32) != 0);

    // Decrypt should recover plaintext
    xr_aes_cbc_decrypt(&ctx, iv, ciphertext, decrypted, 32);
    ASSERT_TRUE(memcmp(plaintext, decrypted, 32) == 0);
}

TEST(crypto_aes_cbc_single_block) {
    uint8_t key[32] = {0};
    uint8_t iv[16] = {0};
    uint8_t plain[16] = {0};
    uint8_t cipher[16], recovered[16];

    XrAESContext ctx;
    xr_aes_init(&ctx, key, 256);
    xr_aes_cbc_encrypt(&ctx, iv, plain, cipher, 16);
    xr_aes_cbc_decrypt(&ctx, iv, cipher, recovered, 16);

    ASSERT_TRUE(memcmp(plain, recovered, 16) == 0);
}

/* ========== Random Bytes ========== */

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

/* ========== Bytes to Hex ========== */

TEST(crypto_bytes_to_hex) {
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    char hex[16];
    xr_bytes_to_hex(data, 4, hex);
    ASSERT_STR_EQ(hex, "deadbeef");
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Crypto - MD5");
RUN_TEST(crypto_md5_empty);
RUN_TEST(crypto_md5_abc);
RUN_TEST(crypto_md5_hello);

RUN_TEST_SUITE("Crypto - SHA-1");
RUN_TEST(crypto_sha1_empty);
RUN_TEST(crypto_sha1_abc);

RUN_TEST_SUITE("Crypto - SHA-256");
RUN_TEST(crypto_sha256_empty);
RUN_TEST(crypto_sha256_abc);
RUN_TEST(crypto_sha256_long);

RUN_TEST_SUITE("Crypto - SHA-512");
RUN_TEST(crypto_sha512_empty);
RUN_TEST(crypto_sha512_abc);

RUN_TEST_SUITE("Crypto - HMAC-SHA256");
RUN_TEST(crypto_hmac_sha256_basic);

RUN_TEST_SUITE("Crypto - AES-256-CBC");
RUN_TEST(crypto_aes_cbc_roundtrip);
RUN_TEST(crypto_aes_cbc_single_block);

RUN_TEST_SUITE("Crypto - Random");
RUN_TEST(crypto_random_bytes);
RUN_TEST(crypto_random_bytes_zero);

RUN_TEST_SUITE("Crypto - Utility");
RUN_TEST(crypto_bytes_to_hex);

TEST_MAIN_END()
