/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * base64.h - Base64 native helpers and module loader
 *
 * KEY CONCEPT:
 *   Public module functions live in stdlib/base64/base64.xr. This header
 *   exposes only native helper calls needed by WS internals plus the loader.
 */

#ifndef XR_STDLIB_BASE64_H
#define XR_STDLIB_BASE64_H

#include "../../src/base/xdefs.h"

/* ========== Native helpers ========== */

// Encode data to standard Base64 (caller must free with xr_free)
char *base64_encode_alloc(const unsigned char *data, size_t len, size_t *out_len);

// URL-safe Base64 encoding (+ -> -, / -> _, no padding; caller must free with xr_free)
char *base64_encode_url_alloc(const unsigned char *data, size_t len, size_t *out_len);

// Decode standard Base64 to binary data (caller must free with xr_free)
unsigned char *base64_decode_alloc(const char *data, size_t len, size_t *out_len);

// Decode URL-safe Base64 to binary data (caller must free with xr_free)
unsigned char *base64_decode_url_alloc(const char *data, size_t len, size_t *out_len);

// Validate Base64 string (checks characters and length)
bool base64_is_valid_bytes(const char *data, size_t len);

/* ========== Module Loader ========== */

struct XrVMRuntime;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_base64(struct XrVMRuntime *isolate);

#endif  // XR_STDLIB_BASE64_H
