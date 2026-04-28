/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * encoding.h - Encoding conversion standard library
 *
 * KEY CONCEPT:
 *   Hex and UTF-16 encoding/decoding. UTF-8 operations delegate to
 *   the core xutf8.h module (no duplication).
 *
 * WHY THIS DESIGN:
 *   - Hex encode/decode are encoding-specific, not needed in core
 *   - UTF-16 is rarely needed in core, lives here as stdlib
 *   - UTF-8 is core functionality, reused from src/object/xutf8.h
 */

#ifndef XR_STDLIB_ENCODING_H
#define XR_STDLIB_ENCODING_H

#include "../../src/base/xdefs.h"

/* ========== Hex Encoding/Decoding ========== */

XR_FUNC int xr_hex_encode(const uint8_t *data, size_t len, char *output);
XR_FUNC int xr_hex_decode(const char *hex, size_t len, uint8_t *output);
XR_FUNC bool xr_hex_valid(const char *hex, size_t len);

/* ========== UTF-16 Encoding/Decoding ========== */

typedef enum {
    XR_UTF16_LE = 0,  // Little-endian
    XR_UTF16_BE = 1   // Big-endian
} XrUtf16Endian;

XR_FUNC int xr_utf16_encode(const uint8_t *utf8, size_t utf8_len, uint8_t *output, size_t out_cap,
                            XrUtf16Endian endian);

XR_FUNC int xr_utf16_decode(const uint8_t *utf16, size_t utf16_len, uint8_t *output, size_t out_cap,
                            XrUtf16Endian endian);

XR_FUNC int xr_utf16_encoded_len(const uint8_t *utf8, size_t utf8_len);

// Compute how many UTF-8 bytes the given UTF-16 buffer would occupy
// once decoded. Renamed from the ambiguous `xr_utf8_decoded_len`: the
// new name makes the direction (UTF-16 → UTF-8) explicit.
XR_FUNC int xr_utf16_to_utf8_len(const uint8_t *utf16, size_t utf16_len, XrUtf16Endian endian);

/* ========== Module Loading ========== */

struct XrayIsolate;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_encoding(struct XrayIsolate *isolate);

#endif  // XR_STDLIB_ENCODING_H
