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

static inline bool xr_crypto_core_hash(XrCryptoCoreHashAlg alg, const uint8_t *data, size_t len,
                                       uint8_t digest[64], size_t *out_len) {
    if ((!data && len != 0) || !digest)
        return false;

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
