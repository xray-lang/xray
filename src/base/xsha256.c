/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsha256.c - Always-available SHA-256 identity primitive
 */

#include "xsha256.h"
#include "xchecks.h"
#include <string.h>

#define XR_SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define XR_SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define XR_SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define XR_SHA256_SIG0(x) (XR_SHA256_ROTR(x, 7) ^ XR_SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define XR_SHA256_SIG1(x) (XR_SHA256_ROTR(x, 17) ^ XR_SHA256_ROTR(x, 19) ^ ((x) >> 10))
#define XR_SHA256_EP0(x) (XR_SHA256_ROTR(x, 2) ^ XR_SHA256_ROTR(x, 13) ^ XR_SHA256_ROTR(x, 22))
#define XR_SHA256_EP1(x) (XR_SHA256_ROTR(x, 6) ^ XR_SHA256_ROTR(x, 11) ^ XR_SHA256_ROTR(x, 25))

static const uint32_t xr_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void xr_sha256_transform(XrSHA256Context *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t) block[i * 4] << 24) | ((uint32_t) block[i * 4 + 1] << 16) |
               ((uint32_t) block[i * 4 + 2] << 8) | (uint32_t) block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++)
        w[i] = XR_SHA256_SIG1(w[i - 2]) + w[i - 7] + XR_SHA256_SIG0(w[i - 15]) + w[i - 16];

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + XR_SHA256_EP1(e) + XR_SHA256_CH(e, f, g) + xr_sha256_k[i] + w[i];
        uint32_t t2 = XR_SHA256_EP0(a) + XR_SHA256_MAJ(a, b, c);
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
    XR_DCHECK(ctx != NULL, "xr_sha256_init: NULL ctx");
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
    size_t index = (size_t) (ctx->count & 0x3f);
    ctx->count += len;
    size_t part_len = 64 - index;
    size_t i = 0;
    if (len >= part_len) {
        memcpy(&ctx->buffer[index], data, part_len);
        xr_sha256_transform(ctx, ctx->buffer);
        for (i = part_len; i + 63 < len; i += 64)
            xr_sha256_transform(ctx, &data[i]);
        index = 0;
    }
    if (len > i)
        memcpy(&ctx->buffer[index], &data[i], len - i);
}

void xr_sha256_final(XrSHA256Context *ctx, uint8_t digest[32]) {
    XR_DCHECK(ctx != NULL, "xr_sha256_final: NULL ctx");
    XR_DCHECK(digest != NULL, "xr_sha256_final: NULL digest");
    uint8_t padding[64] = {0x80};
    uint64_t bits = ctx->count * 8;
    size_t index = (size_t) (ctx->count & 0x3f);
    size_t pad_len = index < 56 ? 56 - index : 120 - index;
    xr_sha256_update(ctx, padding, pad_len);
    uint8_t length[8];
    for (int i = 0; i < 8; i++)
        length[7 - i] = (uint8_t) (bits >> (i * 8));
    xr_sha256_update(ctx, length, sizeof(length));
    for (int i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t) (ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t) (ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t) (ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t) ctx->state[i];
    }
}

void xr_sha256(const uint8_t *data, size_t len, uint8_t digest[32]) {
    XR_DCHECK(data != NULL || len == 0, "xr_sha256: NULL data");
    XR_DCHECK(digest != NULL, "xr_sha256: NULL digest");
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, data, len);
    xr_sha256_final(&ctx, digest);
}

#undef XR_SHA256_ROTR
#undef XR_SHA256_CH
#undef XR_SHA256_MAJ
#undef XR_SHA256_SIG0
#undef XR_SHA256_SIG1
#undef XR_SHA256_EP0
#undef XR_SHA256_EP1
