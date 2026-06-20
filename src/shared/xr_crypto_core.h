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
