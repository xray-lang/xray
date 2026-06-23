/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_crypto_core.h - Shared freestanding crypto utilities
 */

#ifndef XR_CRYPTO_CORE_H
#define XR_CRYPTO_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../base/xdefs.h"

#ifndef XR_CRYPTO_CONTEXT_TYPES_DEFINED
#define XR_CRYPTO_CONTEXT_TYPES_DEFINED
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
#endif

XR_FUNC void xr_md5(const uint8_t *data, size_t len, uint8_t digest[16]);
XR_FUNC void xr_sha1(const uint8_t *data, size_t len, uint8_t digest[20]);
XR_FUNC void xr_sha256(const uint8_t *data, size_t len, uint8_t digest[32]);
XR_FUNC void xr_sha512(const uint8_t *data, size_t len, uint8_t digest[64]);

XR_FUNC void xr_hmac_md5(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                         uint8_t digest[16]);
XR_FUNC void xr_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                          uint8_t digest[20]);
XR_FUNC void xr_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data,
                            size_t data_len, uint8_t digest[32]);
XR_FUNC void xr_hmac_sha512(const uint8_t *key, size_t key_len, const uint8_t *data,
                            size_t data_len, uint8_t digest[64]);

XR_FUNC void xr_bytes_to_hex(const uint8_t *bytes, size_t len, char *output);
XR_FUNC int xr_hex_to_bytes(const char *hex, uint8_t *output, size_t max_len);
XR_FUNC void xr_secure_wipe(void *ptr, size_t len);
XR_FUNC void xr_aes_init(XrAESContext *ctx, const uint8_t *key, int key_bits);
XR_FUNC void xr_aes_cbc_encrypt(XrAESContext *ctx, const uint8_t *iv, const uint8_t *input,
                                uint8_t *output, size_t len);
XR_FUNC void xr_aes_cbc_decrypt(XrAESContext *ctx, const uint8_t *iv, const uint8_t *input,
                                uint8_t *output, size_t len);

typedef enum XrCryptoCoreHashAlg {
    XR_CRYPTO_CORE_HASH_NONE = 0,
    XR_CRYPTO_CORE_HASH_MD5,
    XR_CRYPTO_CORE_HASH_SHA1,
    XR_CRYPTO_CORE_HASH_SHA256,
    XR_CRYPTO_CORE_HASH_SHA512,
} XrCryptoCoreHashAlg;

static inline bool xr_crypto_core_alg_eq(const char *data, size_t len, const char *lit) {
    size_t lit_len = strlen(lit);
    return data && len == lit_len && memcmp(data, lit, lit_len) == 0;
}

static inline XrCryptoCoreHashAlg xr_crypto_core_hash_alg_from_name(const char *data, size_t len) {
    if (xr_crypto_core_alg_eq(data, len, "md5"))
        return XR_CRYPTO_CORE_HASH_MD5;
    if (xr_crypto_core_alg_eq(data, len, "sha1"))
        return XR_CRYPTO_CORE_HASH_SHA1;
    if (xr_crypto_core_alg_eq(data, len, "sha256"))
        return XR_CRYPTO_CORE_HASH_SHA256;
    if (xr_crypto_core_alg_eq(data, len, "sha512"))
        return XR_CRYPTO_CORE_HASH_SHA512;
    return XR_CRYPTO_CORE_HASH_NONE;
}

static inline size_t xr_crypto_core_hash_digest_len(XrCryptoCoreHashAlg alg) {
    switch (alg) {
        case XR_CRYPTO_CORE_HASH_MD5:
            return 16;
        case XR_CRYPTO_CORE_HASH_SHA1:
            return 20;
        case XR_CRYPTO_CORE_HASH_SHA256:
            return 32;
        case XR_CRYPTO_CORE_HASH_SHA512:
            return 64;
        case XR_CRYPTO_CORE_HASH_NONE:
        default:
            return 0;
    }
}

static inline const uint8_t *xr_crypto_core_bytes_or_empty(const uint8_t *data) {
    static const uint8_t empty = 0;
    return data ? data : &empty;
}

static inline bool xr_crypto_core_hash(XrCryptoCoreHashAlg alg, const uint8_t *data, size_t len,
                                       uint8_t digest[64], size_t *out_len) {
    if ((!data && len != 0) || !digest)
        return false;
    data = xr_crypto_core_bytes_or_empty(data);

    size_t digest_len = xr_crypto_core_hash_digest_len(alg);
    if (digest_len == 0)
        return false;

    switch (alg) {
        case XR_CRYPTO_CORE_HASH_MD5:
            xr_md5(data, len, digest);
            break;
        case XR_CRYPTO_CORE_HASH_SHA1:
            xr_sha1(data, len, digest);
            break;
        case XR_CRYPTO_CORE_HASH_SHA256:
            xr_sha256(data, len, digest);
            break;
        case XR_CRYPTO_CORE_HASH_SHA512:
            xr_sha512(data, len, digest);
            break;
        case XR_CRYPTO_CORE_HASH_NONE:
        default:
            return false;
    }

    if (out_len)
        *out_len = digest_len;
    return true;
}

static inline bool xr_crypto_core_hmac(XrCryptoCoreHashAlg alg, const uint8_t *key, size_t key_len,
                                       const uint8_t *data, size_t data_len, uint8_t digest[64],
                                       size_t *out_len) {
    if ((!key && key_len != 0) || (!data && data_len != 0) || !digest)
        return false;
    key = xr_crypto_core_bytes_or_empty(key);
    data = xr_crypto_core_bytes_or_empty(data);

    size_t digest_len = xr_crypto_core_hash_digest_len(alg);
    if (digest_len == 0)
        return false;

    switch (alg) {
        case XR_CRYPTO_CORE_HASH_MD5:
            xr_hmac_md5(key, key_len, data, data_len, digest);
            break;
        case XR_CRYPTO_CORE_HASH_SHA1:
            xr_hmac_sha1(key, key_len, data, data_len, digest);
            break;
        case XR_CRYPTO_CORE_HASH_SHA256:
            xr_hmac_sha256(key, key_len, data, data_len, digest);
            break;
        case XR_CRYPTO_CORE_HASH_SHA512:
            xr_hmac_sha512(key, key_len, data, data_len, digest);
            break;
        case XR_CRYPTO_CORE_HASH_NONE:
        default:
            return false;
    }

    if (out_len)
        *out_len = digest_len;
    return true;
}

static inline bool xr_crypto_core_digest_hex(const uint8_t *digest, size_t digest_len, char *out,
                                             size_t out_cap) {
    if ((!digest && digest_len != 0) || !out || digest_len > 64)
        return false;
    size_t hex_len = digest_len * 2;
    if (out_cap < hex_len + 1)
        return false;
    xr_bytes_to_hex(digest, digest_len, out);
    return true;
}

static inline bool xr_crypto_core_aes_encrypt_plan(size_t plain_len, size_t *out_padded_len,
                                                   size_t *out_hex_len) {
    size_t pad = 16 - (plain_len % 16);
    if (plain_len > SIZE_MAX - pad)
        return false;
    size_t padded_len = plain_len + pad;
    if (padded_len > SIZE_MAX - 16)
        return false;
    size_t out_bytes = 16 + padded_len;
    if (out_bytes > SIZE_MAX / 2)
        return false;
    if (out_padded_len)
        *out_padded_len = padded_len;
    if (out_hex_len)
        *out_hex_len = out_bytes * 2;
    return true;
}

static inline bool xr_crypto_core_aes_decrypt_plan(size_t cipher_hex_len, size_t *out_raw_len,
                                                   size_t *out_cipher_len) {
    if (cipher_hex_len < 64 || (cipher_hex_len % 2) != 0)
        return false;
    size_t raw_len = cipher_hex_len / 2;
    size_t cipher_len = raw_len - 16;
    if (cipher_len == 0 || (cipher_len % 16) != 0)
        return false;
    if (out_raw_len)
        *out_raw_len = raw_len;
    if (out_cipher_len)
        *out_cipher_len = cipher_len;
    return true;
}

static inline int xr_crypto_core_hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static inline bool xr_crypto_core_hex_to_bytes(const char *hex, size_t hex_len, uint8_t *out,
                                               size_t out_cap) {
    if ((!hex && hex_len != 0) || !out || (hex_len % 2) != 0 || hex_len / 2 > out_cap)
        return false;
    for (size_t i = 0; i < hex_len / 2; i++) {
        int hi = xr_crypto_core_hex_digit(hex[i * 2]);
        int lo = xr_crypto_core_hex_digit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t) ((hi << 4) | lo);
    }
    return true;
}

static inline bool xr_crypto_core_aes_encrypt_hex(const uint8_t *key, size_t key_len,
                                                  const uint8_t *plain, size_t plain_len,
                                                  const uint8_t iv[16], uint8_t *padded,
                                                  size_t padded_cap, uint8_t *cipher,
                                                  size_t cipher_cap, char *hex, size_t hex_cap) {
    if ((!key && key_len != 0) || (!plain && plain_len != 0) || !iv || !padded || !cipher || !hex)
        return false;
    key = xr_crypto_core_bytes_or_empty(key);
    plain = xr_crypto_core_bytes_or_empty(plain);

    size_t padded_len = 0;
    size_t hex_len = 0;
    if (!xr_crypto_core_aes_encrypt_plan(plain_len, &padded_len, &hex_len))
        return false;
    if (padded_cap < padded_len || cipher_cap < padded_len || hex_cap < hex_len + 1)
        return false;

    uint8_t aes_key[32];
    xr_sha256(key, key_len, aes_key);

    uint8_t pad = (uint8_t) (16 - (plain_len % 16));
    if (plain_len != 0)
        memcpy(padded, plain, plain_len);
    memset(padded + plain_len, pad, pad);

    XrAESContext ctx;
    xr_aes_init(&ctx, aes_key, 256);
    xr_aes_cbc_encrypt(&ctx, iv, padded, cipher, padded_len);

    xr_bytes_to_hex(iv, 16, hex);
    xr_bytes_to_hex(cipher, padded_len, hex + 32);
    hex[hex_len] = '\0';

    xr_secure_wipe(aes_key, sizeof(aes_key));
    xr_secure_wipe(&ctx, sizeof(ctx));
    return true;
}

static inline bool xr_crypto_core_aes_decrypt_hex(const uint8_t *key, size_t key_len,
                                                  const char *cipher_hex, size_t cipher_hex_len,
                                                  uint8_t *raw, size_t raw_cap, uint8_t *plain,
                                                  size_t plain_cap, size_t *out_plain_len) {
    if ((!key && key_len != 0) || (!cipher_hex && cipher_hex_len != 0) || !raw || !plain)
        return false;
    key = xr_crypto_core_bytes_or_empty(key);

    size_t raw_len = 0;
    size_t cipher_len = 0;
    if (!xr_crypto_core_aes_decrypt_plan(cipher_hex_len, &raw_len, &cipher_len))
        return false;
    if (raw_cap < raw_len || plain_cap < cipher_len)
        return false;
    if (!xr_crypto_core_hex_to_bytes(cipher_hex, cipher_hex_len, raw, raw_cap))
        return false;

    uint8_t aes_key[32];
    xr_sha256(key, key_len, aes_key);

    XrAESContext ctx;
    xr_aes_init(&ctx, aes_key, 256);
    xr_aes_cbc_decrypt(&ctx, raw, raw + 16, plain, cipher_len);

    uint8_t pad = plain[cipher_len - 1];
    volatile uint8_t bad = 0;
    bad |= (uint8_t) (((unsigned) pad - 1) >> 8);
    bad |= (uint8_t) (((unsigned) 16 - pad) >> 8);
    for (int i = 0; i < 16; i++) {
        uint8_t b = plain[cipher_len - 1 - i];
        int cmp = ((int) pad - 1 - i) >> 31;
        bad |= (uint8_t) ((~cmp) & (b ^ pad));
    }

    xr_secure_wipe(aes_key, sizeof(aes_key));
    xr_secure_wipe(&ctx, sizeof(ctx));
    if (bad)
        return false;

    if (out_plain_len)
        *out_plain_len = cipher_len - pad;
    return true;
}

static inline bool xr_crypto_core_timing_safe_equal(const char *a, size_t a_len, const char *b,
                                                    size_t b_len) {
    if ((!a && a_len != 0) || (!b && b_len != 0))
        return false;
    volatile uint8_t diff = (a_len != b_len) ? 1 : 0;
    size_t min_len = a_len < b_len ? a_len : b_len;
    for (size_t i = 0; i < min_len; i++)
        diff |= (uint8_t) (((uint8_t) a[i]) ^ ((uint8_t) b[i]));
    return diff == 0;
}

#endif  // XR_CRYPTO_CORE_H
