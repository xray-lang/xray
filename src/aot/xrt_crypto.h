/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_crypto.h - Freestanding AOT crypto helpers
 */

#ifndef XRT_CRYPTO_H
#define XRT_CRYPTO_H

#include "../shared/xr_crypto_core.h"
#include "xrt_arc.h"
#include "xrt_value.h"

typedef void (*xrt_crypto_hash_fn)(const uint8_t *data, size_t len, uint8_t *digest);
typedef void (*xrt_crypto_hmac_fn)(const uint8_t *key, size_t key_len, const uint8_t *data,
                                   size_t data_len, uint8_t *digest);

static inline XrValue xrt_crypto_hex_result(const uint8_t *digest, size_t digest_len) {
    if (!digest || digest_len > 64)
        return XR_NULL_VAL;
    size_t hex_len = digest_len * 2;
    char hex[129];
    xr_bytes_to_hex(digest, digest_len, hex);
    XrValue result = xrt_str_alloc(hex_len);
    memcpy(xr_str_buf(result), hex, hex_len);
    xr_str_buf(result)[hex_len] = '\0';
    return result;
}

static inline XrValue xrt_crypto_hash_string(const char *data, int64_t len, xrt_crypto_hash_fn hash,
                                             size_t digest_len) {
    if ((!data && len != 0) || len < 0 || !hash || digest_len > 64)
        return XR_NULL_VAL;
    uint8_t digest[64];
    hash((const uint8_t *) data, (size_t) len, digest);
    return xrt_crypto_hex_result(digest, digest_len);
}

static inline XrValue xrt_crypto_md5(const char *data, int64_t len) {
    return xrt_crypto_hash_string(data, len, (xrt_crypto_hash_fn) xr_md5, 16);
}

static inline XrValue xrt_crypto_sha1(const char *data, int64_t len) {
    return xrt_crypto_hash_string(data, len, (xrt_crypto_hash_fn) xr_sha1, 20);
}

static inline XrValue xrt_crypto_sha256(const char *data, int64_t len) {
    return xrt_crypto_hash_string(data, len, (xrt_crypto_hash_fn) xr_sha256, 32);
}

static inline XrValue xrt_crypto_sha512(const char *data, int64_t len) {
    return xrt_crypto_hash_string(data, len, (xrt_crypto_hash_fn) xr_sha512, 64);
}

static inline bool xrt_crypto_alg_eq(const char *data, int64_t len, const char *lit) {
    size_t lit_len = strlen(lit);
    return data && len == (int64_t) lit_len && memcmp(data, lit, lit_len) == 0;
}

static inline XrValue xrt_crypto_hmac_with(xrt_crypto_hmac_fn hmac, size_t digest_len,
                                           const char *key, int64_t key_len, const char *data,
                                           int64_t data_len) {
    if (!hmac || digest_len > 64 || (!key && key_len != 0) || (!data && data_len != 0) ||
        key_len < 0 || data_len < 0)
        return XR_NULL_VAL;
    uint8_t digest[64];
    hmac((const uint8_t *) key, (size_t) key_len, (const uint8_t *) data, (size_t) data_len,
         digest);
    return xrt_crypto_hex_result(digest, digest_len);
}

static inline XrValue xrt_crypto_hmac(const char *algo, int64_t algo_len, const char *key,
                                      int64_t key_len, const char *data, int64_t data_len) {
    if ((!algo && algo_len != 0) || algo_len < 0)
        return XR_NULL_VAL;
    if (xrt_crypto_alg_eq(algo, algo_len, "sha256"))
        return xrt_crypto_hmac_with((xrt_crypto_hmac_fn) xr_hmac_sha256, 32, key, key_len, data,
                                    data_len);
    if (xrt_crypto_alg_eq(algo, algo_len, "md5"))
        return xrt_crypto_hmac_with((xrt_crypto_hmac_fn) xr_hmac_md5, 16, key, key_len, data,
                                    data_len);
    if (xrt_crypto_alg_eq(algo, algo_len, "sha1"))
        return xrt_crypto_hmac_with((xrt_crypto_hmac_fn) xr_hmac_sha1, 20, key, key_len, data,
                                    data_len);
    if (xrt_crypto_alg_eq(algo, algo_len, "sha512"))
        return xrt_crypto_hmac_with((xrt_crypto_hmac_fn) xr_hmac_sha512, 64, key, key_len, data,
                                    data_len);
    return XR_NULL_VAL;
}

static inline XrValue xrt_crypto_timing_safe_equal(const char *a, int64_t a_len, const char *b,
                                                   int64_t b_len) {
    if (a_len < 0 || b_len < 0)
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(xr_crypto_core_timing_safe_equal(a, (size_t) a_len, b, (size_t) b_len));
}

#endif  // XRT_CRYPTO_H
