/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ws_base64.h - Private Base64 helpers for the WebSocket standard module
 *
 * KEY CONCEPT:
 *   Public module functions live in stdlib/base64/base64.xr. This header
 *   exposes only native helper calls needed by WS internals plus the loader.
 */

#ifndef XR_STDLIB_WS_BASE64_H
#define XR_STDLIB_WS_BASE64_H

#include "../../src/base/xdefs.h"

/* ========== Native helpers ========== */

// Encode data to standard Base64 (caller must free with xr_free)
char *base64_encode_alloc(const unsigned char *data, size_t len, size_t *out_len);

// Decode standard Base64 to binary data (caller must free with xr_free)
unsigned char *base64_decode_alloc(const char *data, size_t len, size_t *out_len);

#endif  // XR_STDLIB_WS_BASE64_H
