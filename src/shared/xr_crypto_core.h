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
#include "../base/xsha256.h"

#ifndef XR_CRYPTO_CONTEXT_TYPES_DEFINED
#define XR_CRYPTO_CONTEXT_TYPES_DEFINED
#endif

XR_FUNC void xr_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data,
                            size_t data_len, uint8_t digest[32]);

XR_FUNC void xr_bytes_to_hex(const uint8_t *bytes, size_t len, char *output);
XR_FUNC void xr_secure_wipe(void *ptr, size_t len);

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

static inline bool xr_crypto_core_bytes_hex(const uint8_t *bytes, size_t len, char *out,
                                            size_t out_cap) {
    if ((!bytes && len != 0) || !out || len > (SIZE_MAX - 1) / 2)
        return false;
    size_t hex_len = len * 2;
    if (out_cap < hex_len + 1)
        return false;
    xr_bytes_to_hex(bytes, len, out);
    return true;
}

/* ========== Authenticated Encryption (AES-256-CBC + HMAC-SHA256) ==========
 *
 * crypto.encrypt/decrypt use authenticated encryption so ciphertext cannot be
 * silently tampered with (the old bare CBC output was malleable — a bit-flip in
 * the ciphertext flips the corresponding plaintext bit undetected). We use the
 * Encrypt-then-MAC construction (the provably-secure ordering): CBC-encrypt,
 * then HMAC-SHA256 over IV||ciphertext. Decryption verifies the tag in constant
 * time BEFORE touching the ciphertext, so a forged/altered message is rejected
 * without running the cipher (also closes the CBC padding-oracle surface).
 *
 * Wire format (hex): IV(16) || ciphertext(16*n) || tag(32).
 *
 * The user key is stretched into two independent subkeys via HMAC-SHA256 as a
 * PRF (domain-separated), so the same key never drives both the cipher and MAC.
 */
#define XR_AEAD_TAG_LEN 32
#define XR_AEAD_IV_LEN 16
#define XR_AEAD_OVERHEAD (XR_AEAD_IV_LEN + XR_AEAD_TAG_LEN)

// Encrypt `plain` and write hex(IV || ciphertext || tag) into `hex`.
// `padded` is scratch of at least `padded_len` bytes (PKCS7 buffer);
// `raw` is scratch of at least (IV + padded_len + TAG) bytes (assembled output).
// Verify the tag (constant time), then decrypt. Returns false on any tampering,
// wrong key, or malformed input — never leaks whether padding was the failure.
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
