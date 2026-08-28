/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * crypto.c - Cryptographic functions implementation
 *
 * KEY CONCEPT:
 *   Pure C implementation of cryptographic primitives. No external library
 *   dependencies - all algorithms (MD5, SHA-1, SHA-256, HMAC, AES) are
 *   implemented from scratch for portability.
 */

#include "crypto.h"
#include "../../src/shared/xr_crypto_core.h"
#include "../../src/base/xchecks.h"
#include "../../src/base/xmalloc.h"
#ifndef XR_CRYPTO_CORE_ONLY
#include "../../stdlib/common.h"
#include "../../src/os/os_random.h"
#include "../../src/base/xglobal_indices.h"
#include "../../src/runtime/class/xbuiltin_enum_error.h"
#include "../../src/runtime/core/xr_runtime_core.h"
#include "../../src/runtime/mem/xalloc_unified.h"
#include "../../src/runtime/object/xpanic_info.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/vm/xvm.h"
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ========== Utility Macros ========== */

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROTL64(x, n) (((x) << (n)) | ((x) >> (64 - (n))))
#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

/* ========== Secure Memory Wipe ========== */

XR_FUNC void xr_secure_wipe(void *ptr, size_t len) {
#if defined(XR_OS_MACOS)
    memset_s(ptr, len, 0, len);
#elif defined(__GLIBC__)
    explicit_bzero(ptr, len);
#else
    volatile uint8_t *p = (volatile uint8_t *) ptr;
    while (len--)
        *p++ = 0;
#endif
}

/* ========== HMAC Implementation ========== */

typedef void (*HashFn)(const uint8_t *data, size_t len, uint8_t *digest);

/*
 * Generic HMAC computation using function pointers.
 * block_size: hash block size (64 for MD5/SHA1/SHA256, 128 for SHA512)
 * digest_size: hash output size in bytes
 */
static void hmac_compute(HashFn hash, int block_size, int digest_size, const uint8_t *key,
                         size_t key_len, const uint8_t *data, size_t data_len, uint8_t *digest) {
    uint8_t k[128] = {0};
    uint8_t ipad[128], opad[128];
    uint8_t inner[64];

    if ((int) key_len > block_size) {
        hash(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }

    for (int i = 0; i < block_size; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    size_t inner_len = (size_t) block_size + data_len;
    size_t outer_len = (size_t) block_size + (size_t) digest_size;

    uint8_t stack_buf[4096];
    uint8_t *inner_buf =
        (inner_len <= sizeof(stack_buf)) ? stack_buf : (uint8_t *) xr_malloc(inner_len);
    if (!inner_buf) {
        memset(digest, 0, digest_size);
        return;
    }

    memcpy(inner_buf, ipad, block_size);
    memcpy(inner_buf + block_size, data, data_len);
    hash(inner_buf, inner_len, inner);

    if (inner_buf != stack_buf)
        xr_free(inner_buf);

    // opad || inner_hash (always fits in stack)
    uint8_t outer_buf[192];  // 128 + 64 max
    memcpy(outer_buf, opad, block_size);
    memcpy(outer_buf + block_size, inner, digest_size);
    hash(outer_buf, outer_len, digest);

    xr_secure_wipe(k, sizeof(k));
    xr_secure_wipe(ipad, sizeof(ipad));
    xr_secure_wipe(opad, sizeof(opad));
    xr_secure_wipe(inner, sizeof(inner));
    xr_secure_wipe(outer_buf, sizeof(outer_buf));
}

static void hash_sha256_wrapper(const uint8_t *data, size_t len, uint8_t *digest) {
    xr_sha256(data, len, digest);
}
XR_FUNC void xr_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data,
                            size_t data_len, uint8_t digest[32]) {
    hmac_compute(hash_sha256_wrapper, 64, 32, key, key_len, data, data_len, digest);
}

/* ========== AES Implementation ========== */

static const uint8_t aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static const uint8_t aes_inv_sbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d};

static const uint8_t aes_rcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10,
                                     0x20, 0x40, 0x80, 0x1b, 0x36};

// GF(2^8) multiplication used in MixColumns
static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1)
            p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi)
            a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

XR_FUNC void xr_aes_init(XrAESContext *ctx, const uint8_t *key, int key_bits) {
    XR_DCHECK(ctx != NULL, "xr_aes_init: NULL ctx");
    XR_DCHECK(key != NULL, "xr_aes_init: NULL key");
    XR_DCHECK(key_bits == 128 || key_bits == 192 || key_bits == 256,
              "xr_aes_init: invalid key_bits");
    int nk, nr;
    switch (key_bits) {
        case 128:
            nk = 4;
            nr = 10;
            break;
        case 192:
            nk = 6;
            nr = 12;
            break;
        case 256:
            nk = 8;
            nr = 14;
            break;
        default:
            memset(ctx, 0, sizeof(*ctx));
            return;
    }
    ctx->rounds = nr;

    // Copy key into first nk words
    for (int i = 0; i < nk; i++) {
        ctx->round_key[i] = ((uint32_t) key[4 * i] << 24) | ((uint32_t) key[4 * i + 1] << 16) |
                            ((uint32_t) key[4 * i + 2] << 8) | (uint32_t) key[4 * i + 3];
    }

    // Key expansion
    for (int i = nk; i < 4 * (nr + 1); i++) {
        uint32_t temp = ctx->round_key[i - 1];
        if (i % nk == 0) {
            // RotWord + SubWord + Rcon
            temp = ((uint32_t) aes_sbox[(temp >> 16) & 0xff] << 24) |
                   ((uint32_t) aes_sbox[(temp >> 8) & 0xff] << 16) |
                   ((uint32_t) aes_sbox[temp & 0xff] << 8) |
                   (uint32_t) aes_sbox[(temp >> 24) & 0xff];
            temp ^= (uint32_t) aes_rcon[i / nk] << 24;
        } else if (nk > 6 && i % nk == 4) {
            // Extra SubWord for AES-256
            temp = ((uint32_t) aes_sbox[(temp >> 24) & 0xff] << 24) |
                   ((uint32_t) aes_sbox[(temp >> 16) & 0xff] << 16) |
                   ((uint32_t) aes_sbox[(temp >> 8) & 0xff] << 8) |
                   (uint32_t) aes_sbox[temp & 0xff];
        }
        ctx->round_key[i] = ctx->round_key[i - nk] ^ temp;
    }
}

static void aes_add_round_key(uint8_t state[16], const uint32_t *rk) {
    for (int i = 0; i < 4; i++) {
        state[i * 4] ^= (uint8_t) (rk[i] >> 24);
        state[i * 4 + 1] ^= (uint8_t) (rk[i] >> 16);
        state[i * 4 + 2] ^= (uint8_t) (rk[i] >> 8);
        state[i * 4 + 3] ^= (uint8_t) (rk[i]);
    }
}

static void aes_sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++)
        state[i] = aes_sbox[state[i]];
}

static void aes_inv_sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++)
        state[i] = aes_inv_sbox[state[i]];
}

// AES state is column-major: state[row + 4*col]
static void aes_shift_rows(uint8_t s[16]) {
    uint8_t t;
    // Row 1: shift left 1
    t = s[1];
    s[1] = s[5];
    s[5] = s[9];
    s[9] = s[13];
    s[13] = t;
    // Row 2: shift left 2
    t = s[2];
    s[2] = s[10];
    s[10] = t;
    t = s[6];
    s[6] = s[14];
    s[14] = t;
    // Row 3: shift left 3 (= shift right 1)
    t = s[15];
    s[15] = s[11];
    s[11] = s[7];
    s[7] = s[3];
    s[3] = t;
}

static void aes_inv_shift_rows(uint8_t s[16]) {
    uint8_t t;
    // Row 1: shift right 1
    t = s[13];
    s[13] = s[9];
    s[9] = s[5];
    s[5] = s[1];
    s[1] = t;
    // Row 2: shift right 2
    t = s[2];
    s[2] = s[10];
    s[10] = t;
    t = s[6];
    s[6] = s[14];
    s[14] = t;
    // Row 3: shift right 3 (= shift left 1)
    t = s[3];
    s[3] = s[7];
    s[7] = s[11];
    s[11] = s[15];
    s[15] = t;
}

static void aes_mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
        s[4 * c] = gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3;
        s[4 * c + 1] = a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3;
        s[4 * c + 2] = a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3);
        s[4 * c + 3] = gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2);
    }
}

static void aes_inv_mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
        s[4 * c] = gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9);
        s[4 * c + 1] = gf_mul(a0, 9) ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13);
        s[4 * c + 2] = gf_mul(a0, 13) ^ gf_mul(a1, 9) ^ gf_mul(a2, 14) ^ gf_mul(a3, 11);
        s[4 * c + 3] = gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9) ^ gf_mul(a3, 14);
    }
}

static void aes_encrypt_block(const XrAESContext *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    memcpy(state, in, 16);

    aes_add_round_key(state, &ctx->round_key[0]);
    for (int r = 1; r < ctx->rounds; r++) {
        aes_sub_bytes(state);
        aes_shift_rows(state);
        aes_mix_columns(state);
        aes_add_round_key(state, &ctx->round_key[r * 4]);
    }
    aes_sub_bytes(state);
    aes_shift_rows(state);
    aes_add_round_key(state, &ctx->round_key[ctx->rounds * 4]);

    memcpy(out, state, 16);
}

static void aes_decrypt_block(const XrAESContext *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    memcpy(state, in, 16);

    aes_add_round_key(state, &ctx->round_key[ctx->rounds * 4]);
    for (int r = ctx->rounds - 1; r >= 1; r--) {
        aes_inv_shift_rows(state);
        aes_inv_sub_bytes(state);
        aes_add_round_key(state, &ctx->round_key[r * 4]);
        aes_inv_mix_columns(state);
    }
    aes_inv_shift_rows(state);
    aes_inv_sub_bytes(state);
    aes_add_round_key(state, &ctx->round_key[0]);

    memcpy(out, state, 16);
}

XR_FUNC void xr_aes_cbc_encrypt(XrAESContext *ctx, const uint8_t *iv, const uint8_t *input,
                                uint8_t *output, size_t len) {
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for (size_t i = 0; i < len; i += 16) {
        uint8_t block[16];
        for (int j = 0; j < 16; j++)
            block[j] = input[i + j] ^ prev[j];
        aes_encrypt_block(ctx, block, &output[i]);
        memcpy(prev, &output[i], 16);
    }
}

XR_FUNC void xr_aes_cbc_decrypt(XrAESContext *ctx, const uint8_t *iv, const uint8_t *input,
                                uint8_t *output, size_t len) {
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for (size_t i = 0; i < len; i += 16) {
        uint8_t decrypted[16];
        aes_decrypt_block(ctx, &input[i], decrypted);
        for (int j = 0; j < 16; j++)
            output[i + j] = decrypted[j] ^ prev[j];
        memcpy(prev, &input[i], 16);
    }
}

/* ========== Utility Functions ========== */

static const char hex_chars[] = "0123456789abcdef";

XR_FUNC void xr_bytes_to_hex(const uint8_t *bytes, size_t len, char *output) {
    XR_DCHECK(bytes != NULL || len == 0, "xr_bytes_to_hex: NULL bytes");
    XR_DCHECK(output != NULL, "xr_bytes_to_hex: NULL output");
    for (size_t i = 0; i < len; i++) {
        output[i * 2] = hex_chars[(bytes[i] >> 4) & 0xF];
        output[i * 2 + 1] = hex_chars[bytes[i] & 0xF];
    }
    output[len * 2] = '\0';
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

XR_FUNC int xr_hex_to_bytes(const char *hex, uint8_t *output, size_t max_len) {
    size_t len = strlen(hex);
    if (len % 2 != 0 || len / 2 > max_len)
        return -1;
    for (size_t i = 0; i < len / 2; i++) {
        int hi = hex_digit(hex[i * 2]);
        int lo = hex_digit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        output[i] = (uint8_t) ((hi << 4) | lo);
    }
    return (int) (len / 2);
}

#ifndef XR_CRYPTO_CORE_ONLY

/* ========== Module Bindings ========== */

static void crypto_publish_builtin_enum_error(XrVMRuntime *iso, int builtin_index,
                                              uint32_t member_index, const char *fallback_message) {
    XrBuiltinEnumErrorResult result = xr_builtin_enum_error_construct(
        iso ? xr_isolate_get_runtime_core(iso) : NULL, builtin_index, member_index);
    if (result.status == XR_BUILTIN_ENUM_ERROR_OK) {
        XrValue error = result.value;
        XrExecutionErrorPublishStatus publish = xr_exec_context_publish_error_owned(
            iso ? xr_isolate_get_runtime_core(iso) : NULL, &error);
        if (publish == XR_EXEC_ERROR_PUBLISH_OK)
            return;
        xr_rc_release_value(xr_current_coro_heap(), error);
        error = xr_null();
        if (publish == XR_EXEC_ERROR_PUBLISH_CHANNEL_OCCUPIED)
            return;
    }
    XrValue exc = xr_panic_info_newf(iso, XR_ERR_INTERNAL, "%s",
                                     fallback_message ? fallback_message
                                                      : "failed to construct typed crypto error");
    xr_vm_throw_exception(iso, exc);
}

static XrValue crypto_random_bytes(XrVMRuntime *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_INT(args[0]))
        return xr_null();
    int len = (int) XR_TO_INT(args[0]);
    if (len <= 0 || len > 1024) {
        crypto_publish_builtin_enum_error(isolate, XR_GLOBAL_VAR_CRYPTO_ERROR, 0,
                                          "crypto.randomBytes invalid length");
        return xr_null();
    }
    uint8_t buf[1024];
    char hex[2049];
    xr_random_bytes(buf, len);
    if (!xr_crypto_core_bytes_hex(buf, len, hex, sizeof(hex)))
        return xr_null();
    return xr_string_value(xr_string_new(isolate, hex, len * 2));
}

static XrValue crypto_uuid(XrVMRuntime *isolate, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    uint8_t bytes[16];
    xr_random_bytes(bytes, 16);
    char uuid[37];
    if (!xr_crypto_core_uuid_v4_write(bytes, uuid, sizeof(uuid)))
        return xr_null();
    return xr_string_value(xr_string_new(isolate, uuid, 36));
}

/*
 * crypto.encrypt(key, plaintext) -> hex string
 * Authenticated encryption: AES-256-CBC (PKCS7) then HMAC-SHA256 over
 * IV||ciphertext (Encrypt-then-MAC). The user key is stretched into
 * independent cipher/MAC subkeys. Output is hex(IV(16) || ciphertext || tag(32)),
 * so any tampering is detected on decrypt instead of silently corrupting data.
 */
static XrValue crypto_encrypt(XrVMRuntime *isolate, XrValue *args, int nargs) {
    if (nargs < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]))
        return xr_null();
    XrString *key_str = XR_TO_STRING(args[0]);
    XrString *plain_str = XR_TO_STRING(args[1]);

    size_t padded_len = 0;
    size_t hex_len = 0;
    if (!xr_crypto_core_aead_encrypt_plan(plain_str->length, &padded_len, &hex_len) ||
        hex_len > UINT32_MAX)
        return xr_null();
    size_t raw_len = XR_AEAD_OVERHEAD + padded_len;  // IV + ciphertext + tag

    uint8_t iv[16];
    xr_random_bytes(iv, 16);

    uint8_t stack_plain[4096];
    uint8_t *padded =
        (padded_len <= sizeof(stack_plain)) ? stack_plain : (uint8_t *) xr_malloc(padded_len);
    if (!padded)
        return xr_null();

    uint8_t stack_raw[4096];
    uint8_t *raw = (raw_len <= sizeof(stack_raw)) ? stack_raw : (uint8_t *) xr_malloc(raw_len);
    if (!raw) {
        if (padded != stack_plain)
            xr_free(padded);
        return xr_null();
    }

    char stack_hex[8193];
    char *hex = (hex_len + 1 <= sizeof(stack_hex)) ? stack_hex : (char *) xr_malloc(hex_len + 1);
    if (!hex) {
        if (padded != stack_plain)
            xr_free(padded);
        if (raw != stack_raw)
            xr_free(raw);
        return xr_null();
    }

    if (!xr_crypto_core_aead_encrypt_hex(
            (const uint8_t *) XR_STRING_CHARS(key_str), key_str->length,
            (const uint8_t *) XR_STRING_CHARS(plain_str), plain_str->length, iv, padded, padded_len,
            raw, raw_len, hex, hex_len + 1)) {
        if (padded != stack_plain)
            xr_free(padded);
        if (raw != stack_raw)
            xr_free(raw);
        if (hex != stack_hex)
            xr_free(hex);
        return xr_null();
    }

    XrValue result = xr_string_value(xr_string_new(isolate, hex, (uint32_t) hex_len));

    if (padded != stack_plain)
        xr_free(padded);
    if (raw != stack_raw)
        xr_free(raw);
    if (hex != stack_hex)
        xr_free(hex);
    return result;
}

/*
 * crypto.decrypt(key, ciphertext_hex) -> plaintext string
 * Reverse of crypto.encrypt: verify the HMAC-SHA256 tag in constant time
 * FIRST (returns null on any mismatch — tamper/wrong key), then AES-256-CBC
 * decrypt and strip PKCS7 padding.
 */
static XrValue crypto_decrypt(XrVMRuntime *isolate, XrValue *args, int nargs) {
    if (nargs < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]))
        return xr_null();
    XrString *key_str = XR_TO_STRING(args[0]);
    XrString *cipher_hex_str = XR_TO_STRING(args[1]);

    size_t raw_len = 0;
    size_t cipher_len = 0;
    if (!xr_crypto_core_aead_decrypt_plan(cipher_hex_str->length, &raw_len, &cipher_len)) {
        crypto_publish_builtin_enum_error(isolate, XR_GLOBAL_VAR_CRYPTO_ERROR, 0,
                                          "crypto.decrypt invalid ciphertext length");
        return xr_null();
    }

    uint8_t stack_raw[4096];
    uint8_t *raw = (raw_len <= sizeof(stack_raw)) ? stack_raw : (uint8_t *) xr_malloc(raw_len);
    if (!raw)
        return xr_null();

    uint8_t stack_plain[4096];
    uint8_t *plain =
        (cipher_len <= sizeof(stack_plain)) ? stack_plain : (uint8_t *) xr_malloc(cipher_len);
    if (!plain) {
        if (raw != stack_raw)
            xr_free(raw);
        return xr_null();
    }

    size_t plain_len = 0;
    if (!xr_crypto_core_aead_decrypt_hex((const uint8_t *) XR_STRING_CHARS(key_str),
                                         key_str->length, XR_STRING_CHARS(cipher_hex_str),
                                         cipher_hex_str->length, raw, raw_len, plain, cipher_len,
                                         &plain_len) ||
        plain_len > UINT32_MAX) {
        if (raw != stack_raw)
            xr_free(raw);
        if (plain != stack_plain)
            xr_free(plain);
        return xr_null();
    }

    XrValue result =
        xr_string_value(xr_string_new(isolate, (const char *) plain, (uint32_t) plain_len));

    if (raw != stack_raw)
        xr_free(raw);
    if (plain != stack_plain)
        xr_free(plain);
    return result;
}

// Constant-time comparison to prevent timing attacks
static XrValue crypto_timing_safe_equal(XrVMRuntime *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]))
        return xr_bool(false);
    XrString *a = XR_TO_STRING(args[0]);
    XrString *b = XR_TO_STRING(args[1]);
    bool ok = xr_crypto_core_timing_safe_equal(XR_STRING_CHARS(a), a->length, XR_STRING_CHARS(b),
                                               b->length);
    return xr_bool(ok);
}

#define XR_STDLIB_VM_BIND_MODULE_CRYPTO 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_CRYPTO

XR_FUNC XrModule *xr_native_module_create_crypto(XrVMRuntime *isolate) {
    XrModule *mod = xr_module_create_native(isolate, "crypto");
    if (!mod)
        return NULL;

    xr_stdlib_vm_bind_crypto_generated(isolate, mod);

    return mod;
}

#endif /* XR_CRYPTO_CORE_ONLY */
