/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * base64.c - Base64 C API + pure-Xray module loader
 *
 * KEY CONCEPT:
 *   The base64 module's user-facing functions (encode/decode/encodeUrl/...) are
 *   pure Xray, defined in stdlib/base64/base64.xr. This file keeps only the
 *   C-level xr_base64_* API (built on xr_base64_core) that other C modules
 *   (ws, http_proxy) link against, plus the loader that registers the pure-Xray
 *   module so `import base64` resolves as stdlib.
 */

#include "base64.h"
#include "../../src/base/xchecks.h"
#include "../../src/base/xmalloc.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/shared/xr_base64_core.h"

/* ========== Encoding/decoding helpers ========== */

static char *base64_encode_internal(const unsigned char *data, size_t len, bool url_safe,
                                    bool padding, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    size_t encoded_len = 0;
    if (!xr_base64_core_encoded_len(len, padding, &encoded_len))
        return NULL;
    char *output = (char *) xr_malloc(encoded_len + 1);
    if (!output)
        return NULL;
    if (!xr_base64_core_encode(data, len, url_safe, padding, output, out_len)) {
        xr_free(output);
        return NULL;
    }
    return output;
}

static unsigned char *base64_decode_internal(const char *data, size_t len, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    size_t decoded_len = 0;
    if (!xr_base64_core_decoded_len(data, len, &decoded_len))
        return NULL;
    unsigned char *output = (unsigned char *) xr_malloc(decoded_len + 1);
    if (!output)
        return NULL;
    if (!xr_base64_core_decode(data, len, output, out_len)) {
        xr_free(output);
        return NULL;
    }
    return output;
}

/* ========== C-level API (linked by ws / http_proxy) ========== */

XR_FUNC char *xr_base64_encode(const unsigned char *data, size_t len, size_t *out_len) {
    return base64_encode_internal(data, len, false, true, out_len);
}

XR_FUNC char *xr_base64_encode_url(const unsigned char *data, size_t len, size_t *out_len) {
    return base64_encode_internal(data, len, true, false, out_len);
}

XR_FUNC unsigned char *xr_base64_decode(const char *data, size_t len, size_t *out_len) {
    return base64_decode_internal(data, len, out_len);
}

XR_FUNC unsigned char *xr_base64_decode_url(const char *data, size_t len, size_t *out_len) {
    // The decode table already handles both standard (+/) and URL-safe (-_).
    return base64_decode_internal(data, len, out_len);
}

XR_FUNC bool xr_base64_is_valid(const char *data, size_t len) {
    return xr_base64_core_is_valid(data, len);
}

/* ========== Module loader (pure-Xray module) ========== */

XR_FUNC XrModule *xr_load_module_base64(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_base64: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "base64");
    if (!module)
        return NULL;

    module->requires_script = true;
    module->loaded = true;
    return module;
}
