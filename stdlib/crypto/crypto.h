/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * crypto.h - Runtime cryptographic support used by private providers
 *
 * KEY CONCEPT:
 *   The Xray-facing algorithms are defined only by stdlib/crypto/crypto.xr.
 *   This header exposes runtime/toolchain primitives used by private native
 *   leaves and internal cluster authentication.
 */

#ifndef XR_STDLIB_CRYPTO_H
#define XR_STDLIB_CRYPTO_H

#include "../../src/base/xdefs.h"
#include "../../src/shared/xr_crypto_core.h"

/* Runtime helpers live in xr_crypto_core.h so private providers and
 * freestanding AOT support share one declaration surface. */

/* ========== Hash Functions ========== */

/*
 * MD5 hash functions.
 * Output: 128-bit (16 bytes) digest.
 * NOTE: MD5 is cryptographically broken, use only for compatibility.
 */

/*
 * SHA-1 hash functions.
 * Output: 160-bit (20 bytes) digest.
 * NOTE: SHA-1 is considered weak, use SHA-256 for security-critical applications.
 */

/*
 * SHA-256 hash functions.
 * Output: 256-bit (32 bytes) digest.
 * Recommended for general-purpose cryptographic hashing.
 */
XR_FUNC void xr_sha256_init(XrSHA256Context *ctx);
XR_FUNC void xr_sha256_update(XrSHA256Context *ctx, const uint8_t *data, size_t len);
XR_FUNC void xr_sha256_final(XrSHA256Context *ctx, uint8_t digest[32]);
XR_FUNC void xr_sha256(const uint8_t *data, size_t len, uint8_t digest[32]);

/*
 * SHA-512 hash functions.
 * Output: 512-bit (64 bytes) digest.
 * Uses 64-bit operations, suitable for large data or when extra security margin needed.
 */

/* ========== HMAC (Hash-based Message Authentication Code) ========== */

/*
 * HMAC functions provide message authentication using a secret key.
 * Used to verify both data integrity and authenticity.
 *
 * Parameters:
 *   key, key_len   - Secret key (any length, will be hashed if > block size)
 *   data, data_len - Message to authenticate
 *   digest         - Output buffer (size depends on hash algorithm)
 */

// HMAC-MD5: 128-bit output

// HMAC-SHA1: 160-bit output

// HMAC-SHA256: 256-bit output (recommended)
XR_FUNC void xr_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data,
                            size_t data_len, uint8_t digest[32]);

// HMAC-SHA512: 512-bit output

/* ========== AES Encryption ========== */

/*
 * AES (Advanced Encryption Standard) symmetric encryption.
 * Supports key sizes: 128, 192, or 256 bits.
 */

/*
 * AES-CBC (Cipher Block Chaining) mode encryption.
 * IMPORTANT: Input length must be multiple of 16 bytes (use PKCS7 padding).
 *
 * Parameters:
 *   ctx    - Initialized AES context
 *   iv     - 16-byte initialization vector (must be unique per message)
 *   input  - Plaintext (must be 16-byte aligned)
 *   output - Ciphertext buffer (same size as input)
 *   len    - Input length (must be multiple of 16)
 */

/* ========== Utility Functions ========== */

// Convert bytes to lowercase hex string
// Output buffer must have at least len*2+1 bytes
XR_FUNC void xr_bytes_to_hex(const uint8_t *bytes, size_t len, char *output);

// Securely wipe a memory region so the compiler cannot elide it.
// Uses explicit_bzero / memset_s where available,
// falling back to a volatile write loop.
// Use this to scrub keys, nonces, and handshake proofs before the
// buffer falls out of scope.
XR_FUNC void xr_secure_wipe(void *ptr, size_t len);

#endif  // XR_STDLIB_CRYPTO_H
