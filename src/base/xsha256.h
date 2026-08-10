/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsha256.h - Always-available SHA-256 identity primitive
 *
 * KEY CONCEPT:
 *   Compiler identities and artifact digests must not depend on optional
 *   standard-library modules. This low-level implementation is the single
 *   SHA-256 owner used by both the compiler and the crypto package.
 */

#ifndef XSHA256_H
#define XSHA256_H

#include "xdefs.h"
#include <stddef.h>
#include <stdint.h>

typedef struct XrSHA256Context {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} XrSHA256Context;

XR_FUNC void xr_sha256_init(XrSHA256Context *ctx);
XR_FUNC void xr_sha256_update(XrSHA256Context *ctx, const uint8_t *data, size_t len);
XR_FUNC void xr_sha256_final(XrSHA256Context *ctx, uint8_t digest[32]);
XR_FUNC void xr_sha256(const uint8_t *data, size_t len, uint8_t digest[32]);

#endif  // XSHA256_H
