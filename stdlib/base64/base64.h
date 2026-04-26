/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * base64.h - Base64 encoding/decoding module
 *
 * KEY CONCEPT:
 *   Provides standard Base64 and URL-safe Base64 encoding/decoding.
 *   Both C-level API and xray module interface are available.
 */

#ifndef XR_STDLIB_BASE64_H
#define XR_STDLIB_BASE64_H

#include <stddef.h>
#include <stdbool.h>

/* ========== C-level API ========== */

// Encode data to standard Base64 (caller must free with xr_free)
char *xr_base64_encode(const unsigned char *data, size_t len, size_t *out_len);

// URL-safe Base64 encoding (+ -> -, / -> _, no padding; caller must free with xr_free)
char *xr_base64_encode_url(const unsigned char *data, size_t len, size_t *out_len);

// Decode standard Base64 to binary data (caller must free with xr_free)
unsigned char *xr_base64_decode(const char *data, size_t len, size_t *out_len);

// Decode URL-safe Base64 to binary data (caller must free with xr_free)
unsigned char *xr_base64_decode_url(const char *data, size_t len, size_t *out_len);

// Validate Base64 string (checks characters and length)
bool xr_base64_is_valid(const char *data, size_t len);

/* ========== xray module interface ========== */

#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/gc/xgc.h"
#include "../../src/base/xmalloc.h"

/*
 * Load base64 module. Provides:
 *   - encode(str)           Encode string to Base64
 *   - decode(str)           Decode Base64 to string
 *   - encodeUrl(str)        URL-safe Base64 encoding
 *   - decodeUrl(str)        URL-safe Base64 decoding
 *   - encodeBytes(bytes)    Encode Array<uint8> to Base64 string
 *   - decodeToBytes(str)    Decode Base64 to Array<uint8>
 *   - isValid(str)          Check if string is valid Base64
 */
XrModule *xr_load_module_base64(XrayIsolate *isolate);

#endif
