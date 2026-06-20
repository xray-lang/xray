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

#include <stdio.h>
#include <string.h>

#include "../os/os_random.h"
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

static inline XrValue xrt_crypto_random_bytes(XrValue len_value) {
    if (!XR_IS_INT(len_value))
        return XR_NULL_VAL;
    int64_t len64 = XR_TO_INT(len_value);
    if (len64 <= 0 || len64 > 1024)
        return XR_NULL_VAL;

    size_t len = (size_t) len64;
    uint8_t buf[1024];
    char hex[2049];
    xr_random_bytes(buf, len);
    xr_bytes_to_hex(buf, len, hex);

    XrValue result = xrt_str_alloc(len * 2);
    memcpy(xr_str_buf(result), hex, len * 2);
    xr_str_buf(result)[len * 2] = '\0';
    return result;
}

static inline XrValue xrt_crypto_uuid(void) {
    uint8_t bytes[16];
    xr_random_bytes(bytes, 16);
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    char uuid[37];
    snprintf(uuid, sizeof(uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
             bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
             bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

    XrValue result = xrt_str_alloc(36);
    memcpy(xr_str_buf(result), uuid, 36);
    xr_str_buf(result)[36] = '\0';
    return result;
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
