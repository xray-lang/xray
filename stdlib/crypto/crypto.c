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
#include "../common.h"
#include "../../src/base/xmalloc.h"
#include "../../src/os/os_random.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/object/xstring.h"
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

/* ========== MD5 Constants ========== */

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

static const uint8_t md5_s[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

/* ========== MD5 Implementation ========== */

static void md5_transform(XrMD5Context *ctx, const uint8_t block[64]) {
    uint32_t a = ctx->state[0], b = ctx->state[1];
    uint32_t c = ctx->state[2], d = ctx->state[3];
    uint32_t m[16];

    for (int i = 0; i < 16; i++) {
        m[i] = (uint32_t) block[i * 4] | ((uint32_t) block[i * 4 + 1] << 8) |
               ((uint32_t) block[i * 4 + 2] << 16) | ((uint32_t) block[i * 4 + 3] << 24);
    }

    for (int i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t temp = d;
        d = c;
        c = b;
        b = b + ROTL32(a + f + md5_k[i] + m[g], md5_s[i]);
        a = temp;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

void xr_md5_init(XrMD5Context *ctx) {
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

void xr_md5_update(XrMD5Context *ctx, const uint8_t *data, size_t len) {
    XR_DCHECK(ctx != NULL, "xr_md5_update: NULL ctx");
    XR_DCHECK(data != NULL || len == 0, "xr_md5_update: NULL data");
    uint32_t index = (ctx->count[0] >> 3) & 0x3F;
    ctx->count[0] += (uint32_t) (len << 3);
    if (ctx->count[0] < (uint32_t) (len << 3))
        ctx->count[1]++;
    ctx->count[1] += (uint32_t) (len >> 29);

    size_t part_len = 64 - index, i = 0;
    if (len >= part_len) {
        memcpy(&ctx->buffer[index], data, part_len);
        md5_transform(ctx, ctx->buffer);
        for (i = part_len; i + 63 < len; i += 64)
            md5_transform(ctx, &data[i]);
        index = 0;
    }
    memcpy(&ctx->buffer[index], &data[i], len - i);
}

void xr_md5_final(XrMD5Context *ctx, uint8_t digest[16]) {
    uint8_t padding[64] = {0x80}, bits[8];
    for (int i = 0; i < 4; i++) {
        bits[i] = (uint8_t) (ctx->count[0] >> (i * 8));
        bits[i + 4] = (uint8_t) (ctx->count[1] >> (i * 8));
    }
    uint32_t index = (ctx->count[0] >> 3) & 0x3F;
    size_t pad_len = (index < 56) ? (56 - index) : (120 - index);
    xr_md5_update(ctx, padding, pad_len);
    xr_md5_update(ctx, bits, 8);
    for (int i = 0; i < 4; i++) {
        digest[i * 4] = (uint8_t) (ctx->state[i]);
        digest[i * 4 + 1] = (uint8_t) (ctx->state[i] >> 8);
        digest[i * 4 + 2] = (uint8_t) (ctx->state[i] >> 16);
        digest[i * 4 + 3] = (uint8_t) (ctx->state[i] >> 24);
    }
}

void xr_md5(const uint8_t *data, size_t len, uint8_t digest[16]) {
    XrMD5Context ctx;
    xr_md5_init(&ctx);
    xr_md5_update(&ctx, data, len);
    xr_md5_final(&ctx, digest);
}

/* ========== SHA-256 Constants ========== */

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define SHA256_SIG0(x) (ROTR32(x, 7) ^ ROTR32(x, 18) ^ ((x) >> 3))
#define SHA256_SIG1(x) (ROTR32(x, 17) ^ ROTR32(x, 19) ^ ((x) >> 10))
#define SHA256_EP0(x) (ROTR32(x, 2) ^ ROTR32(x, 13) ^ ROTR32(x, 22))
#define SHA256_EP1(x) (ROTR32(x, 6) ^ ROTR32(x, 11) ^ ROTR32(x, 25))

/* ========== SHA-256 Implementation ========== */

static void sha256_transform(XrSHA256Context *ctx, const uint8_t block[64]) {
    uint32_t w[64], a, b, c, d, e, f, g, h;

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t) block[i * 4] << 24) | ((uint32_t) block[i * 4 + 1] << 16) |
               ((uint32_t) block[i * 4 + 2] << 8) | (uint32_t) block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SHA256_SIG1(w[i - 2]) + w[i - 7] + SHA256_SIG0(w[i - 15]) + w[i - 16];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + SHA256_EP1(e) + CH(e, f, g) + sha256_k[i] + w[i];
        uint32_t t2 = SHA256_EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void xr_sha256_init(XrSHA256Context *ctx) {
    ctx->count = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

void xr_sha256_update(XrSHA256Context *ctx, const uint8_t *data, size_t len) {
    XR_DCHECK(ctx != NULL, "xr_sha256_update: NULL ctx");
    XR_DCHECK(data != NULL || len == 0, "xr_sha256_update: NULL data");
    size_t index = (size_t) (ctx->count & 0x3F);
    ctx->count += len;
    size_t part_len = 64 - index, i = 0;
    if (len >= part_len) {
        memcpy(&ctx->buffer[index], data, part_len);
        sha256_transform(ctx, ctx->buffer);
        for (i = part_len; i + 63 < len; i += 64)
            sha256_transform(ctx, &data[i]);
        index = 0;
    }
    memcpy(&ctx->buffer[index], &data[i], len - i);
}

void xr_sha256_final(XrSHA256Context *ctx, uint8_t digest[32]) {
    uint8_t padding[64] = {0x80};
    uint64_t bits = ctx->count * 8;
    size_t index = (size_t) (ctx->count & 0x3F);
    size_t pad_len = (index < 56) ? (56 - index) : (120 - index);
    xr_sha256_update(ctx, padding, pad_len);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++)
        len_bytes[7 - i] = (uint8_t) (bits >> (i * 8));
    xr_sha256_update(ctx, len_bytes, 8);
    for (int i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t) (ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t) (ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t) (ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t) (ctx->state[i]);
    }
}

void xr_sha256(const uint8_t *data, size_t len, uint8_t digest[32]) {
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, data, len);
    xr_sha256_final(&ctx, digest);
}

/* ========== SHA-1 Implementation ========== */

void xr_sha1_init(XrSHA1Context *ctx) {
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
}

static void sha1_transform(XrSHA1Context *ctx, const uint8_t block[64]) {
    uint32_t w[80], a, b, c, d, e;
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t) block[i * 4] << 24) | ((uint32_t) block[i * 4 + 1] << 16) |
               ((uint32_t) block[i * 4 + 2] << 8) | (uint32_t) block[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++)
        w[i] = ROTL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t temp = ROTL32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROTL32(b, 30);
        b = a;
        a = temp;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

void xr_sha1_update(XrSHA1Context *ctx, const uint8_t *data, size_t len) {
    uint32_t index = (ctx->count[0] >> 3) & 0x3F;
    ctx->count[0] += (uint32_t) (len << 3);
    if (ctx->count[0] < (uint32_t) (len << 3))
        ctx->count[1]++;
    ctx->count[1] += (uint32_t) (len >> 29);
    size_t part_len = 64 - index, i = 0;
    if (len >= part_len) {
        memcpy(&ctx->buffer[index], data, part_len);
        sha1_transform(ctx, ctx->buffer);
        for (i = part_len; i + 63 < len; i += 64)
            sha1_transform(ctx, &data[i]);
        index = 0;
    }
    memcpy(&ctx->buffer[index], &data[i], len - i);
}

void xr_sha1_final(XrSHA1Context *ctx, uint8_t digest[20]) {
    uint8_t padding[64] = {0x80}, bits[8];
    uint64_t total_bits = ((uint64_t) ctx->count[1] << 32) | ctx->count[0];
    for (int i = 0; i < 8; i++)
        bits[7 - i] = (uint8_t) (total_bits >> (i * 8));
    uint32_t index = (ctx->count[0] >> 3) & 0x3F;
    size_t pad_len = (index < 56) ? (56 - index) : (120 - index);
    xr_sha1_update(ctx, padding, pad_len);
    xr_sha1_update(ctx, bits, 8);
    for (int i = 0; i < 5; i++) {
        digest[i * 4] = (uint8_t) (ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t) (ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t) (ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t) (ctx->state[i]);
    }
}

void xr_sha1(const uint8_t *data, size_t len, uint8_t digest[20]) {
    XrSHA1Context ctx;
    xr_sha1_init(&ctx);
    xr_sha1_update(&ctx, data, len);
    xr_sha1_final(&ctx, digest);
}

/* ========== SHA-512 Constants ========== */

static const uint64_t sha512_k[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

#define SHA512_SIG0(x) (ROTR64(x, 1) ^ ROTR64(x, 8) ^ ((x) >> 7))
#define SHA512_SIG1(x) (ROTR64(x, 19) ^ ROTR64(x, 61) ^ ((x) >> 6))
#define SHA512_EP0(x) (ROTR64(x, 28) ^ ROTR64(x, 34) ^ ROTR64(x, 39))
#define SHA512_EP1(x) (ROTR64(x, 14) ^ ROTR64(x, 18) ^ ROTR64(x, 41))

/* ========== SHA-512 Implementation ========== */

static void sha512_transform(XrSHA512Context *ctx, const uint8_t block[128]) {
    uint64_t w[80], a, b, c, d, e, f, g, h;

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint64_t) block[i * 8] << 56) | ((uint64_t) block[i * 8 + 1] << 48) |
               ((uint64_t) block[i * 8 + 2] << 40) | ((uint64_t) block[i * 8 + 3] << 32) |
               ((uint64_t) block[i * 8 + 4] << 24) | ((uint64_t) block[i * 8 + 5] << 16) |
               ((uint64_t) block[i * 8 + 6] << 8) | (uint64_t) block[i * 8 + 7];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = SHA512_SIG1(w[i - 2]) + w[i - 7] + SHA512_SIG0(w[i - 15]) + w[i - 16];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (int i = 0; i < 80; i++) {
        uint64_t t1 = h + SHA512_EP1(e) + CH(e, f, g) + sha512_k[i] + w[i];
        uint64_t t2 = SHA512_EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void xr_sha512_init(XrSHA512Context *ctx) {
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x6a09e667f3bcc908ULL;
    ctx->state[1] = 0xbb67ae8584caa73bULL;
    ctx->state[2] = 0x3c6ef372fe94f82bULL;
    ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->state[4] = 0x510e527fade682d1ULL;
    ctx->state[5] = 0x9b05688c2b3e6c1fULL;
    ctx->state[6] = 0x1f83d9abfb41bd6bULL;
    ctx->state[7] = 0x5be0cd19137e2179ULL;
}

void xr_sha512_update(XrSHA512Context *ctx, const uint8_t *data, size_t len) {
    size_t index = (size_t) (ctx->count[0] & 0x7F);
    ctx->count[0] += len;
    if (ctx->count[0] < len)
        ctx->count[1]++;

    size_t part_len = 128 - index, i = 0;
    if (len >= part_len) {
        memcpy(&ctx->buffer[index], data, part_len);
        sha512_transform(ctx, ctx->buffer);
        for (i = part_len; i + 127 < len; i += 128)
            sha512_transform(ctx, &data[i]);
        index = 0;
    }
    memcpy(&ctx->buffer[index], &data[i], len - i);
}

void xr_sha512_final(XrSHA512Context *ctx, uint8_t digest[64]) {
    uint8_t padding[128] = {0x80};
    uint64_t bits_lo = ctx->count[0] * 8;
    uint64_t bits_hi = ctx->count[1] * 8 + (ctx->count[0] >> 61);
    size_t index = (size_t) (ctx->count[0] & 0x7F);
    size_t pad_len = (index < 112) ? (112 - index) : (240 - index);
    xr_sha512_update(ctx, padding, pad_len);
    uint8_t len_bytes[16];
    for (int i = 0; i < 8; i++)
        len_bytes[7 - i] = (uint8_t) (bits_hi >> (i * 8));
    for (int i = 0; i < 8; i++)
        len_bytes[15 - i] = (uint8_t) (bits_lo >> (i * 8));
    xr_sha512_update(ctx, len_bytes, 16);
    for (int i = 0; i < 8; i++) {
        digest[i * 8] = (uint8_t) (ctx->state[i] >> 56);
        digest[i * 8 + 1] = (uint8_t) (ctx->state[i] >> 48);
        digest[i * 8 + 2] = (uint8_t) (ctx->state[i] >> 40);
        digest[i * 8 + 3] = (uint8_t) (ctx->state[i] >> 32);
        digest[i * 8 + 4] = (uint8_t) (ctx->state[i] >> 24);
        digest[i * 8 + 5] = (uint8_t) (ctx->state[i] >> 16);
        digest[i * 8 + 6] = (uint8_t) (ctx->state[i] >> 8);
        digest[i * 8 + 7] = (uint8_t) (ctx->state[i]);
    }
}

void xr_sha512(const uint8_t *data, size_t len, uint8_t digest[64]) {
    XrSHA512Context ctx;
    xr_sha512_init(&ctx);
    xr_sha512_update(&ctx, data, len);
    xr_sha512_final(&ctx, digest);
}

/* ========== Secure Memory Wipe ========== */

void xr_secure_wipe(void *ptr, size_t len) {
#if defined(__APPLE__)
    memset_s(ptr, len, 0, len);
#elif defined(__GLIBC__)
    explicit_bzero(ptr, len);
#elif defined(_WIN32)
    SecureZeroMemory(ptr, len);
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

static void hash_md5_wrapper(const uint8_t *data, size_t len, uint8_t *digest) {
    xr_md5(data, len, digest);
}
static void hash_sha1_wrapper(const uint8_t *data, size_t len, uint8_t *digest) {
    xr_sha1(data, len, digest);
}
static void hash_sha256_wrapper(const uint8_t *data, size_t len, uint8_t *digest) {
    xr_sha256(data, len, digest);
}
static void hash_sha512_wrapper(const uint8_t *data, size_t len, uint8_t *digest) {
    xr_sha512(data, len, digest);
}

void xr_hmac_md5(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                 uint8_t digest[16]) {
    hmac_compute(hash_md5_wrapper, 64, 16, key, key_len, data, data_len, digest);
}

void xr_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                  uint8_t digest[20]) {
    hmac_compute(hash_sha1_wrapper, 64, 20, key, key_len, data, data_len, digest);
}

void xr_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                    uint8_t digest[32]) {
    hmac_compute(hash_sha256_wrapper, 64, 32, key, key_len, data, data_len, digest);
}

void xr_hmac_sha512(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                    uint8_t digest[64]) {
    hmac_compute(hash_sha512_wrapper, 128, 64, key, key_len, data, data_len, digest);
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

void xr_aes_init(XrAESContext *ctx, const uint8_t *key, int key_bits) {
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

void xr_aes_cbc_encrypt(XrAESContext *ctx, const uint8_t *iv, const uint8_t *input, uint8_t *output,
                        size_t len) {
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

void xr_aes_cbc_decrypt(XrAESContext *ctx, const uint8_t *iv, const uint8_t *input, uint8_t *output,
                        size_t len) {
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

void xr_bytes_to_hex(const uint8_t *bytes, size_t len, char *output) {
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

int xr_hex_to_bytes(const char *hex, uint8_t *output, size_t max_len) {
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

/* ========== Module Bindings ========== */

static XrValue crypto_md5(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0]))
        return xr_null();
    XrString *s = XR_TO_STRING(args[0]);
    uint8_t digest[16];
    xr_md5((const uint8_t *) XR_STRING_CHARS(s), s->length, digest);
    char hex[33];
    xr_bytes_to_hex(digest, 16, hex);
    return xr_string_value(xr_string_new(isolate, hex, 32));
}

static XrValue crypto_sha1(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0]))
        return xr_null();
    XrString *s = XR_TO_STRING(args[0]);
    uint8_t digest[20];
    xr_sha1((const uint8_t *) XR_STRING_CHARS(s), s->length, digest);
    char hex[41];
    xr_bytes_to_hex(digest, 20, hex);
    return xr_string_value(xr_string_new(isolate, hex, 40));
}

static XrValue crypto_sha256(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0]))
        return xr_null();
    XrString *s = XR_TO_STRING(args[0]);
    uint8_t digest[32];
    xr_sha256((const uint8_t *) XR_STRING_CHARS(s), s->length, digest);
    char hex[65];
    xr_bytes_to_hex(digest, 32, hex);
    return xr_string_value(xr_string_new(isolate, hex, 64));
}

static XrValue crypto_hmac(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 3)
        return xr_null();
    if (!XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]) || !XR_IS_STRING(args[2]))
        return xr_null();
    XrString *algo = XR_TO_STRING(args[0]);
    XrString *key = XR_TO_STRING(args[1]);
    XrString *data = XR_TO_STRING(args[2]);

    if (strcmp(XR_STRING_CHARS(algo), "sha256") == 0) {
        uint8_t digest[32];
        xr_hmac_sha256((const uint8_t *) XR_STRING_CHARS(key), key->length,
                       (const uint8_t *) XR_STRING_CHARS(data), data->length, digest);
        char hex[65];
        xr_bytes_to_hex(digest, 32, hex);
        return xr_string_value(xr_string_new(isolate, hex, 64));
    } else if (strcmp(XR_STRING_CHARS(algo), "md5") == 0) {
        uint8_t digest[16];
        xr_hmac_md5((const uint8_t *) XR_STRING_CHARS(key), key->length,
                    (const uint8_t *) XR_STRING_CHARS(data), data->length, digest);
        char hex[33];
        xr_bytes_to_hex(digest, 16, hex);
        return xr_string_value(xr_string_new(isolate, hex, 32));
    } else if (strcmp(XR_STRING_CHARS(algo), "sha1") == 0) {
        uint8_t digest[20];
        xr_hmac_sha1((const uint8_t *) XR_STRING_CHARS(key), key->length,
                     (const uint8_t *) XR_STRING_CHARS(data), data->length, digest);
        char hex[41];
        xr_bytes_to_hex(digest, 20, hex);
        return xr_string_value(xr_string_new(isolate, hex, 40));
    } else if (strcmp(XR_STRING_CHARS(algo), "sha512") == 0) {
        uint8_t digest[64];
        xr_hmac_sha512((const uint8_t *) XR_STRING_CHARS(key), key->length,
                       (const uint8_t *) XR_STRING_CHARS(data), data->length, digest);
        char hex[129];
        xr_bytes_to_hex(digest, 64, hex);
        return xr_string_value(xr_string_new(isolate, hex, 128));
    }
    return xr_null();
}

static XrValue crypto_sha512(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0]))
        return xr_null();
    XrString *s = XR_TO_STRING(args[0]);
    uint8_t digest[64];
    xr_sha512((const uint8_t *) XR_STRING_CHARS(s), s->length, digest);
    char hex[129];
    xr_bytes_to_hex(digest, 64, hex);
    return xr_string_value(xr_string_new(isolate, hex, 128));
}

static XrValue crypto_random_bytes(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_INT(args[0]))
        return xr_null();
    int len = (int) XR_TO_INT(args[0]);
    if (len <= 0 || len > 1024)
        return xr_null();
    uint8_t buf[1024];
    char hex[2049];
    xr_random_bytes(buf, len);
    xr_bytes_to_hex(buf, len, hex);
    return xr_string_value(xr_string_new(isolate, hex, len * 2));
}

static XrValue crypto_uuid(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    uint8_t bytes[16];
    xr_random_bytes(bytes, 16);
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;
    char uuid[37];
    snprintf(uuid, sizeof(uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
             bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
             bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return xr_string_value(xr_string_new(isolate, uuid, 36));
}

/*
 * crypto.encrypt(key, plaintext) -> hex string
 * AES-256-CBC with PKCS7 padding.
 * Key is SHA-256 hashed to 32 bytes. IV is randomly generated and
 * prepended to the ciphertext. Output is hex-encoded (iv + ciphertext).
 */
static XrValue crypto_encrypt(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]))
        return xr_null();
    XrString *key_str = XR_TO_STRING(args[0]);
    XrString *plain_str = XR_TO_STRING(args[1]);

    // Derive 256-bit key via SHA-256
    uint8_t aes_key[32];
    xr_sha256((const uint8_t *) XR_STRING_CHARS(key_str), key_str->length, aes_key);

    // Generate random IV
    uint8_t iv[16];
    xr_random_bytes(iv, 16);

    // PKCS7 padding
    size_t plain_len = plain_str->length;
    uint8_t pad = 16 - (plain_len % 16);
    size_t padded_len = plain_len + pad;

    uint8_t stack_plain[4096];
    uint8_t *padded =
        (padded_len <= sizeof(stack_plain)) ? stack_plain : (uint8_t *) xr_malloc(padded_len);
    if (!padded)
        return xr_null();
    memcpy(padded, XR_STRING_CHARS(plain_str), plain_len);
    memset(padded + plain_len, pad, pad);

    // Encrypt
    uint8_t stack_cipher[4096];
    uint8_t *cipher =
        (padded_len <= sizeof(stack_cipher)) ? stack_cipher : (uint8_t *) xr_malloc(padded_len);
    if (!cipher) {
        if (padded != stack_plain)
            xr_free(padded);
        return xr_null();
    }

    XrAESContext ctx;
    xr_aes_init(&ctx, aes_key, 256);
    xr_aes_cbc_encrypt(&ctx, iv, padded, cipher, padded_len);

    // Output: hex(iv || ciphertext)
    size_t out_bytes = 16 + padded_len;
    size_t hex_len = out_bytes * 2;
    char stack_hex[8193];
    char *hex = (hex_len + 1 <= sizeof(stack_hex)) ? stack_hex : (char *) xr_malloc(hex_len + 1);
    if (!hex) {
        if (padded != stack_plain)
            xr_free(padded);
        if (cipher != stack_cipher)
            xr_free(cipher);
        return xr_null();
    }

    xr_bytes_to_hex(iv, 16, hex);
    xr_bytes_to_hex(cipher, padded_len, hex + 32);

    XrValue result = xr_string_value(xr_string_new(isolate, hex, (uint32_t) hex_len));

    xr_secure_wipe(aes_key, sizeof(aes_key));
    xr_secure_wipe(&ctx, sizeof(ctx));
    if (padded != stack_plain)
        xr_free(padded);
    if (cipher != stack_cipher)
        xr_free(cipher);
    if (hex != stack_hex)
        xr_free(hex);
    return result;
}

/*
 * crypto.decrypt(key, ciphertext_hex) -> plaintext string
 * Reverse of crypto.encrypt: hex decode, extract IV, AES-256-CBC decrypt,
 * remove PKCS7 padding.
 */
static XrValue crypto_decrypt(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]))
        return xr_null();
    XrString *key_str = XR_TO_STRING(args[0]);
    XrString *cipher_hex_str = XR_TO_STRING(args[1]);

    size_t hex_len = cipher_hex_str->length;
    // Must be even, at least 32 hex chars for IV + 32 for one block
    if (hex_len < 64 || hex_len % 2 != 0)
        return xr_null();

    size_t total_bytes = hex_len / 2;
    size_t cipher_len = total_bytes - 16;
    if (cipher_len == 0 || cipher_len % 16 != 0)
        return xr_null();

    // Hex decode
    uint8_t stack_raw[4096];
    uint8_t *raw =
        (total_bytes <= sizeof(stack_raw)) ? stack_raw : (uint8_t *) xr_malloc(total_bytes);
    if (!raw)
        return xr_null();

    if (xr_hex_to_bytes(XR_STRING_CHARS(cipher_hex_str), raw, total_bytes) < 0) {
        if (raw != stack_raw)
            xr_free(raw);
        return xr_null();
    }

    uint8_t *iv = raw;
    uint8_t *cipher = raw + 16;

    // Derive key
    uint8_t aes_key[32];
    xr_sha256((const uint8_t *) XR_STRING_CHARS(key_str), key_str->length, aes_key);

    // Decrypt
    uint8_t stack_plain[4096];
    uint8_t *plain =
        (cipher_len <= sizeof(stack_plain)) ? stack_plain : (uint8_t *) xr_malloc(cipher_len);
    if (!plain) {
        if (raw != stack_raw)
            xr_free(raw);
        return xr_null();
    }

    XrAESContext ctx;
    xr_aes_init(&ctx, aes_key, 256);
    xr_aes_cbc_decrypt(&ctx, iv, cipher, plain, cipher_len);

    // Remove PKCS7 padding (constant-time to prevent padding oracle attacks)
    uint8_t pad = plain[cipher_len - 1];
    volatile uint8_t bad = 0;
    // pad must be in [1, 16]
    bad |= (uint8_t) (((unsigned) pad - 1) >> 8);   // bad if pad == 0
    bad |= (uint8_t) (((unsigned) 16 - pad) >> 8);  // bad if pad > 16
    // Verify all 16 potential padding bytes (fixed iteration count)
    for (int i = 0; i < 16; i++) {
        uint8_t b = plain[cipher_len - 1 - i];
        // cmp = 0 if i < pad (should check), -1 if i >= pad (ignore)
        int cmp = ((int) pad - 1 - i) >> 31;
        bad |= (uint8_t) ((~cmp) & (b ^ pad));
    }
    if (bad) {
        xr_secure_wipe(aes_key, sizeof(aes_key));
        xr_secure_wipe(&ctx, sizeof(ctx));
        if (raw != stack_raw)
            xr_free(raw);
        if (plain != stack_plain)
            xr_free(plain);
        return xr_null();
    }
    size_t plain_len = cipher_len - pad;

    XrValue result =
        xr_string_value(xr_string_new(isolate, (const char *) plain, (uint32_t) plain_len));

    xr_secure_wipe(aes_key, sizeof(aes_key));
    xr_secure_wipe(&ctx, sizeof(ctx));
    if (raw != stack_raw)
        xr_free(raw);
    if (plain != stack_plain)
        xr_free(plain);
    return result;
}

// Constant-time comparison to prevent timing attacks
static XrValue crypto_timing_safe_equal(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]))
        return xr_bool(false);
    XrString *a = XR_TO_STRING(args[0]);
    XrString *b = XR_TO_STRING(args[1]);
    uint32_t len_a = a->length;
    uint32_t len_b = b->length;
    // Length mismatch guarantees false, but still compare all bytes
    // of the shorter string to avoid leaking length info via timing
    volatile uint8_t diff = (len_a != len_b) ? 1 : 0;
    uint32_t min_len = len_a < len_b ? len_a : len_b;
    const char *pa = XR_STRING_CHARS(a);
    const char *pb = XR_STRING_CHARS(b);
    for (uint32_t i = 0; i < min_len; i++) {
        diff |= (uint8_t) (pa[i] ^ pb[i]);
    }
    return diff == 0 ? xr_bool(true) : xr_bool(false);
}

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module crypto

XR_DEFINE_BUILTIN(crypto_md5, "md5", "(data: string): string", "Compute MD5 hash")
XR_DEFINE_BUILTIN(crypto_sha1, "sha1", "(data: string): string", "Compute SHA-1 hash")
XR_DEFINE_BUILTIN(crypto_sha256, "sha256", "(data: string): string", "Compute SHA-256 hash")
XR_DEFINE_BUILTIN(crypto_sha512, "sha512", "(data: string): string", "Compute SHA-512 hash")
XR_DEFINE_BUILTIN(crypto_hmac, "hmac", "(algo: string, key: string, data: string): string",
                  "Compute HMAC")
XR_DEFINE_BUILTIN(crypto_random_bytes, "randomBytes", "(n: int): string", "Generate random bytes")
XR_DEFINE_BUILTIN(crypto_uuid, "uuid", "(): string", "Generate UUID v4")
XR_DEFINE_BUILTIN(crypto_encrypt, "encrypt", "(key: string, plaintext: string): string",
                  "AES-256-CBC encrypt")
XR_DEFINE_BUILTIN(crypto_decrypt, "decrypt", "(key: string, ciphertext: string): string?",
                  "AES-256-CBC decrypt")
XR_DEFINE_BUILTIN(crypto_timing_safe_equal, "timingSafeEqual", "(a: string, b: string): bool",
                  "Constant-time string comparison")

XrModule *xr_load_module_crypto(XrayIsolate *isolate) {
    XrModule *mod = xr_module_create_native(isolate, "crypto");
    if (!mod)
        return NULL;

    XRS_EXPORT(mod, isolate, "md5", crypto_md5);
    XRS_EXPORT(mod, isolate, "sha1", crypto_sha1);
    XRS_EXPORT(mod, isolate, "sha256", crypto_sha256);
    XRS_EXPORT(mod, isolate, "sha512", crypto_sha512);
    XRS_EXPORT(mod, isolate, "hmac", crypto_hmac);
    XRS_EXPORT(mod, isolate, "randomBytes", crypto_random_bytes);
    XRS_EXPORT(mod, isolate, "uuid", crypto_uuid);
    XRS_EXPORT(mod, isolate, "encrypt", crypto_encrypt);
    XRS_EXPORT(mod, isolate, "decrypt", crypto_decrypt);
    XRS_EXPORT(mod, isolate, "timingSafeEqual", crypto_timing_safe_equal);

    mod->loaded = true;
    return mod;
}
