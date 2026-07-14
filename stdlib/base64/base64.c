/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * base64.c - Base64 native helpers + pure-Xray module loader
 *
 * KEY CONCEPT:
 *   The base64 module's user-facing functions (encode/decode/isValid + options)
 *   pure Xray, defined in stdlib/base64/base64.xr. This file keeps only the
 *   small standard Base64 helpers used by WS native handshake code, plus the
 *   loader that registers the pure-Xray module so `import base64` resolves as
 *   stdlib.
 */

#include "base64.h"
#include "../../src/base/xchecks.h"
#include "../../src/base/xmalloc.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"
#include <stdint.h>

/* ========== Encoding/decoding helpers ========== */

static const char BASE64_STD_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static bool base64_encoded_len(size_t len, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (len > (SIZE_MAX / 4) * 3)
        return false;
    if (out_len)
        *out_len = ((len + 2) / 3) * 4;
    return true;
}

static int base64_decode_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return 26 + c - 'a';
    if (c >= '0' && c <= '9')
        return 52 + c - '0';
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static int base64_count_padding(const char *data, size_t len) {
    int pad = 0;
    while (len > 0 && data[len - 1] == '=') {
        pad++;
        len--;
    }
    return pad <= 2 ? pad : -1;
}

static bool base64_decoded_len(const char *data, size_t len, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!data)
        return false;

    int pad = base64_count_padding(data, len);
    if (pad < 0)
        return false;

    size_t unpadded_len = len - (size_t) pad;
    if ((unpadded_len % 4) == 1)
        return false;

    size_t decoded_len = (unpadded_len / 4) * 3;
    size_t tail = unpadded_len % 4;
    if (tail == 2)
        decoded_len += 1;
    else if (tail == 3)
        decoded_len += 2;
    if (out_len)
        *out_len = decoded_len;
    return true;
}

/* ========== Native helpers (linked by ws) ========== */

char *base64_encode_alloc(const unsigned char *data, size_t len, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!data && len != 0)
        return NULL;

    size_t encoded_len = 0;
    if (!base64_encoded_len(len, &encoded_len))
        return NULL;

    char *output = (char *) xr_malloc(encoded_len + 1);
    if (!output)
        return NULL;

    size_t i = 0;
    size_t j = 0;
    while (i + 2 < len) {
        uint32_t n = ((uint32_t) data[i] << 16) | ((uint32_t) data[i + 1] << 8) | data[i + 2];
        output[j++] = BASE64_STD_ALPHABET[(n >> 18) & 0x3F];
        output[j++] = BASE64_STD_ALPHABET[(n >> 12) & 0x3F];
        output[j++] = BASE64_STD_ALPHABET[(n >> 6) & 0x3F];
        output[j++] = BASE64_STD_ALPHABET[n & 0x3F];
        i += 3;
    }

    size_t remaining = len - i;
    if (remaining == 1) {
        uint32_t n = (uint32_t) data[i] << 16;
        output[j++] = BASE64_STD_ALPHABET[(n >> 18) & 0x3F];
        output[j++] = BASE64_STD_ALPHABET[(n >> 12) & 0x3F];
        output[j++] = '=';
        output[j++] = '=';
    } else if (remaining == 2) {
        uint32_t n = ((uint32_t) data[i] << 16) | ((uint32_t) data[i + 1] << 8);
        output[j++] = BASE64_STD_ALPHABET[(n >> 18) & 0x3F];
        output[j++] = BASE64_STD_ALPHABET[(n >> 12) & 0x3F];
        output[j++] = BASE64_STD_ALPHABET[(n >> 6) & 0x3F];
        output[j++] = '=';
    }

    output[j] = '\0';
    if (out_len)
        *out_len = j;
    XR_DCHECK(j == encoded_len, "base64_encode_alloc: encoded length mismatch");
    return output;
}

unsigned char *base64_decode_alloc(const char *data, size_t len, size_t *out_len) {
    if (out_len)
        *out_len = 0;

    size_t decoded_len = 0;
    if (!base64_decoded_len(data, len, &decoded_len))
        return NULL;

    unsigned char *output = (unsigned char *) xr_malloc(decoded_len + 1);
    if (!output)
        return NULL;

    int pad = base64_count_padding(data, len);
    size_t unpadded_len = len - (size_t) pad;
    size_t i = 0;
    size_t j = 0;
    while (i + 4 <= unpadded_len) {
        int a = base64_decode_value((unsigned char) data[i]);
        int b = base64_decode_value((unsigned char) data[i + 1]);
        int c = base64_decode_value((unsigned char) data[i + 2]);
        int d = base64_decode_value((unsigned char) data[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0)
            goto fail;
        output[j++] = (unsigned char) ((a << 2) | (b >> 4));
        output[j++] = (unsigned char) (((b & 15) << 4) | (c >> 2));
        output[j++] = (unsigned char) (((c & 3) << 6) | d);
        i += 4;
    }

    size_t remaining = unpadded_len - i;
    if (remaining >= 2) {
        int a = base64_decode_value((unsigned char) data[i]);
        int b = base64_decode_value((unsigned char) data[i + 1]);
        if (a < 0 || b < 0)
            goto fail;
        output[j++] = (unsigned char) ((a << 2) | (b >> 4));
        if (remaining == 3) {
            int c = base64_decode_value((unsigned char) data[i + 2]);
            if (c < 0)
                goto fail;
            output[j++] = (unsigned char) (((b & 15) << 4) | (c >> 2));
        }
    }

    output[j] = '\0';
    if (out_len)
        *out_len = j;
    XR_DCHECK(j == decoded_len, "base64_decode_alloc: decoded length mismatch");
    return output;

fail:
    xr_free(output);
    if (out_len)
        *out_len = 0;
    return NULL;
}

/* ========== Module loader (pure-Xray module) ========== */

XR_FUNC XrModule *xr_load_module_base64(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_base64: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "base64");
    if (!module)
        return NULL;

    module->requires_script = true;
    return module;
}
