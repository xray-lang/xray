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
