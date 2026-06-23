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

static inline XrValue xrt_crypto_hex_result(const uint8_t *digest, size_t digest_len) {
    if (!digest || digest_len > 64)
        return XR_NULL_VAL;
    size_t hex_len = digest_len * 2;
    char hex[129];
    if (!xr_crypto_core_digest_hex(digest, digest_len, hex, sizeof(hex)))
        return XR_NULL_VAL;
    XrValue result = xrt_str_alloc(hex_len);
    memcpy(xr_str_buf(result), hex, hex_len);
    xr_str_buf(result)[hex_len] = '\0';
    return result;
}

static inline XrValue xrt_crypto_hash_string(const char *data, int64_t len,
                                             XrCryptoCoreHashAlg alg) {
    if ((!data && len != 0) || len < 0)
        return XR_NULL_VAL;
    uint8_t digest[64];
    size_t digest_len = 0;
    if (!xr_crypto_core_hash(alg, (const uint8_t *) data, (size_t) len, digest, &digest_len))
        return XR_NULL_VAL;
    return xrt_crypto_hex_result(digest, digest_len);
}

static inline XrValue xrt_crypto_md5(const char *data, int64_t len) {
    return xrt_crypto_hash_string(data, len, XR_CRYPTO_CORE_HASH_MD5);
}

static inline XrValue xrt_crypto_sha1(const char *data, int64_t len) {
    return xrt_crypto_hash_string(data, len, XR_CRYPTO_CORE_HASH_SHA1);
}

static inline XrValue xrt_crypto_sha256(const char *data, int64_t len) {
    return xrt_crypto_hash_string(data, len, XR_CRYPTO_CORE_HASH_SHA256);
}

static inline XrValue xrt_crypto_sha512(const char *data, int64_t len) {
    return xrt_crypto_hash_string(data, len, XR_CRYPTO_CORE_HASH_SHA512);
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

static inline XrValue xrt_crypto_encrypt(const char *key, int64_t key_len, const char *plain,
                                         int64_t plain_len) {
    if ((!key && key_len != 0) || (!plain && plain_len != 0) || key_len < 0 || plain_len < 0)
        return XR_NULL_VAL;

    size_t input_len = (size_t) plain_len;
    size_t padded_len = 0;
    size_t hex_len = 0;
    if (!xr_crypto_core_aes_encrypt_plan(input_len, &padded_len, &hex_len))
        return XR_NULL_VAL;

    uint8_t iv[16];
    xr_random_bytes(iv, 16);

    uint8_t stack_plain[4096];
    uint8_t *padded =
        (padded_len <= sizeof(stack_plain)) ? stack_plain : (uint8_t *) XRT_MALLOC(padded_len);
    if (!padded)
        return XR_NULL_VAL;

    uint8_t stack_cipher[4096];
    uint8_t *cipher =
        (padded_len <= sizeof(stack_cipher)) ? stack_cipher : (uint8_t *) XRT_MALLOC(padded_len);
    if (!cipher) {
        if (padded != stack_plain)
            XRT_FREE(padded);
        return XR_NULL_VAL;
    }

    XrValue result = xrt_str_alloc(hex_len);
    char *hex = xr_str_buf(result);
    if (!xr_crypto_core_aes_encrypt_hex((const uint8_t *) key, (size_t) key_len,
                                        (const uint8_t *) plain, input_len, iv, padded, padded_len,
                                        cipher, padded_len, hex, hex_len + 1)) {
        xrt_release(result);
        if (padded != stack_plain)
            XRT_FREE(padded);
        if (cipher != stack_cipher)
            XRT_FREE(cipher);
        return XR_NULL_VAL;
    }

    if (padded != stack_plain)
        XRT_FREE(padded);
    if (cipher != stack_cipher)
        XRT_FREE(cipher);
    return result;
}

static inline XrValue xrt_crypto_decrypt(const char *key, int64_t key_len, const char *cipher_hex,
                                         int64_t cipher_hex_len) {
    if ((!key && key_len != 0) || (!cipher_hex && cipher_hex_len != 0) || key_len < 0 ||
        cipher_hex_len < 0)
        return XR_NULL_VAL;

    size_t raw_len = 0;
    size_t cipher_len = 0;
    if (!xr_crypto_core_aes_decrypt_plan((size_t) cipher_hex_len, &raw_len, &cipher_len))
        return XR_NULL_VAL;

    uint8_t stack_raw[4096];
    uint8_t *raw = (raw_len <= sizeof(stack_raw)) ? stack_raw : (uint8_t *) XRT_MALLOC(raw_len);
    if (!raw)
        return XR_NULL_VAL;

    uint8_t stack_plain[4096];
    uint8_t *plain =
        (cipher_len <= sizeof(stack_plain)) ? stack_plain : (uint8_t *) XRT_MALLOC(cipher_len);
    if (!plain) {
        if (raw != stack_raw)
            XRT_FREE(raw);
        return XR_NULL_VAL;
    }

    XrValue result = XR_NULL_VAL;
    size_t plain_out_len = 0;
    if (xr_crypto_core_aes_decrypt_hex((const uint8_t *) key, (size_t) key_len, cipher_hex,
                                       (size_t) cipher_hex_len, raw, raw_len, plain, cipher_len,
                                       &plain_out_len)) {
        result = xrt_str_alloc(plain_out_len);
        memcpy(xr_str_buf(result), plain, plain_out_len);
        xr_str_buf(result)[plain_out_len] = '\0';
    }

    if (raw != stack_raw)
        XRT_FREE(raw);
    if (plain != stack_plain)
        XRT_FREE(plain);
    return result;
}

static inline XrValue xrt_crypto_hmac(const char *algo, int64_t algo_len, const char *key,
                                      int64_t key_len, const char *data, int64_t data_len) {
    if ((!algo && algo_len != 0) || (!key && key_len != 0) || (!data && data_len != 0) ||
        algo_len < 0 || key_len < 0 || data_len < 0)
        return XR_NULL_VAL;

    XrCryptoCoreHashAlg alg = xr_crypto_core_hash_alg_from_name(algo, (size_t) algo_len);
    uint8_t digest[64];
    size_t digest_len = 0;
    if (!xr_crypto_core_hmac(alg, (const uint8_t *) key, (size_t) key_len, (const uint8_t *) data,
                             (size_t) data_len, digest, &digest_len))
        return XR_NULL_VAL;
    return xrt_crypto_hex_result(digest, digest_len);
}

static inline XrValue xrt_crypto_timing_safe_equal(const char *a, int64_t a_len, const char *b,
                                                   int64_t b_len) {
    if (a_len < 0 || b_len < 0)
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(xr_crypto_core_timing_safe_equal(a, (size_t) a_len, b, (size_t) b_len));
}

#endif  // XRT_CRYPTO_H
