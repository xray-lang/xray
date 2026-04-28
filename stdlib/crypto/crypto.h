/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * crypto.h - Cryptographic functions for xray
 *
 * KEY CONCEPT:
 *   Pure C implementation of cryptographic primitives without external
 *   dependencies. Provides hash functions, HMAC, AES encryption, and
 *   cryptographically secure random number generation.
 *
 * XRAY API (script level):
 *   Hash functions (return hex string):
 *     - crypto.md5(data)              MD5 hash (128-bit, 32 hex chars)
 *     - crypto.sha1(data)             SHA-1 hash (160-bit, 40 hex chars)
 *     - crypto.sha256(data)           SHA-256 hash (256-bit, 64 hex chars)
 *     - crypto.sha512(data)           SHA-512 hash (512-bit, 128 hex chars)
 *
 *   HMAC (keyed-hash message authentication code):
 *     - crypto.hmac(algorithm, key, data)
 *       algorithm: "md5" | "sha1" | "sha256" | "sha512"
 *
 *   Security utilities:
 *     - crypto.timingSafeEqual(a, b)  Constant-time string comparison
 *
 *   Random generation:
 *     - crypto.randomBytes(length)    Cryptographically secure random bytes (hex)
 *     - crypto.uuid()                 Generate UUID v4
 *
 *   AES encryption (AES-256-CBC, key hashed via SHA-256, random IV):
 *     - crypto.encrypt(key, plaintext)    Returns hex-encoded iv+ciphertext
 *     - crypto.decrypt(key, ciphertext)   Returns decrypted plaintext or null
 */

#ifndef XR_STDLIB_CRYPTO_H
#define XR_STDLIB_CRYPTO_H

#include "../../src/base/xdefs.h"

/* ========== Internal Data Structures ========== */

/*
 * MD5 context for incremental hashing.
 * Allows processing data in chunks via init/update/final pattern.
 */
typedef struct {
    uint32_t state[4];   // Current hash state (A, B, C, D)
    uint32_t count[2];   // Number of bits processed (low, high)
    uint8_t buffer[64];  // Input buffer for incomplete block
} XrMD5Context;

/*
 * SHA-1 context for incremental hashing.
 * NOTE: SHA-1 is considered weak, use SHA-256 for new applications.
 */
typedef struct {
    uint32_t state[5];   // Current hash state (H0-H4)
    uint32_t count[2];   // Number of bits processed (low, high)
    uint8_t buffer[64];  // Input buffer for incomplete block
} XrSHA1Context;

/*
 * SHA-256 context for incremental hashing.
 * Recommended for general-purpose cryptographic hashing.
 */
typedef struct {
    uint32_t state[8];   // Current hash state (H0-H7)
    uint64_t count;      // Number of bytes processed
    uint8_t buffer[64];  // Input buffer for incomplete block (512-bit blocks)
} XrSHA256Context;

/*
 * SHA-512 context for incremental hashing.
 * Uses 64-bit operations, may be faster on 64-bit platforms.
 */
typedef struct {
    uint64_t state[8];    // Current hash state (H0-H7)
    uint64_t count[2];    // Number of bits processed (low, high)
    uint8_t buffer[128];  // Input buffer for incomplete block (1024-bit blocks)
} XrSHA512Context;

/*
 * AES context for encryption/decryption.
 * Supports AES-128 (10 rounds), AES-192 (12 rounds), AES-256 (14 rounds).
 */
typedef struct {
    uint32_t round_key[60];  // Expanded key schedule (max 60 words for AES-256)
    int rounds;              // Number of rounds: 10, 12, or 14
} XrAESContext;

/* ========== Hash Functions ========== */

/*
 * MD5 hash functions.
 * Output: 128-bit (16 bytes) digest.
 * NOTE: MD5 is cryptographically broken, use only for compatibility.
 */
void xr_md5_init(XrMD5Context *ctx);                                     // Initialize context
void xr_md5_update(XrMD5Context *ctx, const uint8_t *data, size_t len);  // Process data chunk
void xr_md5_final(XrMD5Context *ctx, uint8_t digest[16]);                // Finalize and output
void xr_md5(const uint8_t *data, size_t len, uint8_t digest[16]);        // One-shot hash

/*
 * SHA-1 hash functions.
 * Output: 160-bit (20 bytes) digest.
 * NOTE: SHA-1 is considered weak, use SHA-256 for security-critical applications.
 */
void xr_sha1_init(XrSHA1Context *ctx);
void xr_sha1_update(XrSHA1Context *ctx, const uint8_t *data, size_t len);
void xr_sha1_final(XrSHA1Context *ctx, uint8_t digest[20]);
void xr_sha1(const uint8_t *data, size_t len, uint8_t digest[20]);

/*
 * SHA-256 hash functions.
 * Output: 256-bit (32 bytes) digest.
 * Recommended for general-purpose cryptographic hashing.
 */
void xr_sha256_init(XrSHA256Context *ctx);
void xr_sha256_update(XrSHA256Context *ctx, const uint8_t *data, size_t len);
void xr_sha256_final(XrSHA256Context *ctx, uint8_t digest[32]);
void xr_sha256(const uint8_t *data, size_t len, uint8_t digest[32]);

/*
 * SHA-512 hash functions.
 * Output: 512-bit (64 bytes) digest.
 * Uses 64-bit operations, suitable for large data or when extra security margin needed.
 */
void xr_sha512_init(XrSHA512Context *ctx);
void xr_sha512_update(XrSHA512Context *ctx, const uint8_t *data, size_t len);
void xr_sha512_final(XrSHA512Context *ctx, uint8_t digest[64]);
void xr_sha512(const uint8_t *data, size_t len, uint8_t digest[64]);

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
void xr_hmac_md5(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                 uint8_t digest[16]);

// HMAC-SHA1: 160-bit output
void xr_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                  uint8_t digest[20]);

// HMAC-SHA256: 256-bit output (recommended)
void xr_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                    uint8_t digest[32]);

// HMAC-SHA512: 512-bit output
void xr_hmac_sha512(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                    uint8_t digest[64]);

/* ========== AES Encryption ========== */

/*
 * AES (Advanced Encryption Standard) symmetric encryption.
 * Supports key sizes: 128, 192, or 256 bits.
 */

// Initialize AES context with key
// key_bits: 128, 192, or 256
void xr_aes_init(XrAESContext *ctx, const uint8_t *key, int key_bits);

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
void xr_aes_cbc_encrypt(XrAESContext *ctx, const uint8_t *iv, const uint8_t *input, uint8_t *output,
                        size_t len);

// AES-CBC decryption (same parameters as encrypt)
void xr_aes_cbc_decrypt(XrAESContext *ctx, const uint8_t *iv, const uint8_t *input, uint8_t *output,
                        size_t len);

/* ========== Utility Functions ========== */

// Convert bytes to lowercase hex string
// Output buffer must have at least len*2+1 bytes
void xr_bytes_to_hex(const uint8_t *bytes, size_t len, char *output);

// Convert hex string to bytes
// Returns: number of bytes written, or -1 on error (invalid hex or buffer too small)
int xr_hex_to_bytes(const char *hex, uint8_t *output, size_t max_len);

// Securely wipe a memory region so the compiler cannot elide it.
// Uses explicit_bzero / memset_s / SecureZeroMemory where available,
// falling back to a volatile write loop.
// Use this to scrub keys, nonces, and handshake proofs before the
// buffer falls out of scope.
void xr_secure_wipe(void *ptr, size_t len);

/* ========== Module Loading ========== */

struct XrayIsolate;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_crypto(struct XrayIsolate *isolate);

#endif  // XR_STDLIB_CRYPTO_H
