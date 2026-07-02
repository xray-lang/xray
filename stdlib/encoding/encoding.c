/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * encoding.c - Encoding C API + pure-Xray module loader
 *
 * KEY CONCEPT:
 *   The encoding module's user-facing functions (hex* / utf8* / utf16*) are pure
 *   Xray, defined in stdlib/encoding/encoding.xr. This file keeps only the
 *   C-level xr_hex_* / xr_utf16_* API (built on xr_encoding_core) used by the
 *   hex encoding unit tests, plus the loader that registers the pure-Xray
 *   module so `import encoding` resolves as stdlib.
 */

#include "encoding.h"
#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/shared/xr_encoding_core.h"
#include <limits.h>

/* ========== Hex (C API) ========== */

XR_FUNC int xr_hex_encode(const uint8_t *data, size_t len, char *output) {
    size_t out_len = 0;
    if (!xr_encoding_core_hex_encoded_len(len, &out_len) || out_len > (size_t) INT_MAX)
        return 0;
    if (!xr_encoding_core_hex_encode(data, len, output))
        return 0;
    return (int) out_len;
}

XR_FUNC int xr_hex_decode(const char *hex, size_t len, uint8_t *output) {
    size_t out_len = 0;
    if (!xr_encoding_core_hex_decode(hex, len, output, &out_len) || out_len > (size_t) INT_MAX)
        return -1;
    return (int) out_len;
}

XR_FUNC bool xr_hex_valid(const char *hex, size_t len) {
    return xr_encoding_core_hex_valid(hex, len);
}

/* ========== UTF-16 (C API) ========== */

XR_FUNC int xr_utf16_encode(const uint8_t *utf8, size_t utf8_len, uint8_t *output, size_t out_cap,
                            XrUtf16Endian endian) {
    size_t out_len = 0;
    if (!xr_encoding_core_utf16_encode(
            (const char *) utf8, utf8_len, output, out_cap,
            endian == XR_UTF16_BE ? XR_ENCODING_UTF16_BE : XR_ENCODING_UTF16_LE, &out_len) ||
        out_len > (size_t) INT_MAX)
        return -1;
    return (int) out_len;
}

XR_FUNC int xr_utf16_decode(const uint8_t *utf16, size_t utf16_len, uint8_t *output, size_t out_cap,
                            XrUtf16Endian endian) {
    size_t out_len = 0;
    if (!xr_encoding_core_utf16_decode(
            utf16, utf16_len, (char *) output, out_cap,
            endian == XR_UTF16_BE ? XR_ENCODING_UTF16_BE : XR_ENCODING_UTF16_LE, &out_len) ||
        out_len > (size_t) INT_MAX)
        return -1;
    return (int) out_len;
}

XR_FUNC int xr_utf16_encoded_len(const uint8_t *utf8, size_t utf8_len) {
    size_t out_len = 0;
    if (!xr_encoding_core_utf16_encoded_len((const char *) utf8, utf8_len, &out_len) ||
        out_len > (size_t) INT_MAX)
        return -1;
    return (int) out_len;
}

XR_FUNC int xr_utf16_to_utf8_len(const uint8_t *utf16, size_t utf16_len, XrUtf16Endian endian) {
    size_t out_len = 0;
    if (!xr_encoding_core_utf16_to_utf8_len(
            utf16, utf16_len, endian == XR_UTF16_BE ? XR_ENCODING_UTF16_BE : XR_ENCODING_UTF16_LE,
            &out_len) ||
        out_len > (size_t) INT_MAX)
        return -1;
    return (int) out_len;
}

/* ========== Module loader (pure-Xray module) ========== */

XR_FUNC XrModule *xr_load_module_encoding(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_encoding: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "encoding");
    if (!module)
        return NULL;

    module->requires_script = true;
    module->loaded = true;
    return module;
}
