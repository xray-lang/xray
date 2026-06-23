/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * encoding.c - Character encoding conversion implementation
 *
 * KEY CONCEPT:
 *   Hex and UTF-8 scalar operations delegate to shared core helpers.
 *   UTF-16 encode returns Array<uint8> (binary data, not string).
 */

#include "encoding.h"
#include "../common.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/base/xmalloc.h"
#include "../../src/base/xchecks.h"
#include "../../src/runtime/mem/xheap.h"
#include "../../src/shared/xr_encoding_core.h"
#include <limits.h>
#include <string.h>

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

/* ========== UTF-16 Implementation ========== */

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

/* ========== Helper Functions ========== */

static XrValue make_bytes(XrVMRuntime *X, const uint8_t *data, int len) {
    XrCoroutine *coro = xr_current_coro(X);
    if (!coro)
        return xr_null();
    XrArray *arr = xr_array_with_capacity_typed(coro, len, XR_ELEM_U8);
    if (!arr)
        return xr_null();
    if (len > 0) {
        memcpy(arr->data, data, len);
        arr->length = (int32_t) len;
    }
    return xr_value_from_array(arr);
}

/* ========== xray Binding Functions ========== */

// encoding.hexEncode(str) -> string
static XrValue encoding_hex_encode(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1)
        return xr_null();

    size_t len;
    const char *data = xrs_string_arg(args[0], &len);
    if (!data)
        return xr_null();

    char *output = (char *) xr_malloc(len * 2 + 1);
    if (!output)
        return xr_null();

    xr_hex_encode((const uint8_t *) data, len, output);
    XrValue result = xrs_string_value_n(X, output, len * 2);
    xr_free(output);
    return result;
}

// encoding.hexDecode(hex) -> Array<uint8>
static XrValue encoding_hex_decode(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1)
        return xr_null();

    size_t len;
    const char *hex = xrs_string_arg(args[0], &len);
    if (!hex)
        return xr_null();

    uint8_t *output = (uint8_t *) xr_malloc(len / 2 + 1);
    if (!output)
        return xr_null();

    int out_len = xr_hex_decode(hex, len, output);
    if (out_len < 0) {
        xr_free(output);
        return xr_null();
    }

    XrValue result = make_bytes(X, output, out_len);
    xr_free(output);
    return result;
}

// encoding.hexDecodeString(hex) -> string?
static XrValue encoding_hex_decode_string(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1)
        return xr_null();

    size_t len;
    const char *hex = xrs_string_arg(args[0], &len);
    if (!hex)
        return xr_null();

    uint8_t *output = (uint8_t *) xr_malloc(len / 2 + 1);
    if (!output)
        return xr_null();

    int out_len = xr_hex_decode(hex, len, output);
    if (out_len < 0) {
        xr_free(output);
        return xr_null();
    }

    XrValue result = xrs_string_value_n(X, (char *) output, out_len);
    xr_free(output);
    return result;
}

// encoding.hexValid(hex) -> bool
static XrValue encoding_hex_valid(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    if (nargs < 1)
        return xr_bool(false);

    size_t len;
    const char *hex = xrs_string_arg(args[0], &len);
    if (!hex)
        return xr_bool(false);

    return xr_bool(xr_hex_valid(hex, len));
}

// encoding.utf8Valid(str) -> bool
static XrValue encoding_utf8_valid(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    if (nargs < 1)
        return xr_bool(false);

    size_t len;
    const char *str = xrs_string_arg(args[0], &len);
    if (!str)
        return xr_bool(false);

    return xr_bool(xr_encoding_core_utf8_valid(str, len));
}

// encoding.utf8Count(str) -> int
static XrValue encoding_utf8_count(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    if (nargs < 1)
        return xr_int(0);

    size_t len;
    const char *str = xrs_string_arg(args[0], &len);
    if (!str)
        return xr_int(0);

    return xr_int((int64_t) xr_encoding_core_utf8_count(str, len));
}

// encoding.utf8ByteLength(str) -> int
static XrValue encoding_utf8_byte_length(XrVMRuntime *X, XrValue *args, int nargs) {
    (void) X;
    if (nargs < 1)
        return xr_int(0);

    size_t len;
    const char *str = xrs_string_arg(args[0], &len);
    if (!str)
        return xr_int(0);

    return xr_int((int64_t) len);
}

static XrUtf16Endian parse_endian_arg(XrValue *args, int nargs) {
    if (nargs >= 2 && XR_IS_INT(args[1])) {
        return XR_TO_INT(args[1]) == XR_UTF16_BE ? XR_UTF16_BE : XR_UTF16_LE;
    }
    return XR_UTF16_LE;
}

// encoding.utf16Encode(str, endian?) -> Array<uint8>
static XrValue encoding_utf16_encode(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1)
        return xr_null();

    size_t len;
    const char *str = xrs_string_arg(args[0], &len);
    if (!str)
        return xr_null();

    XrUtf16Endian endian = parse_endian_arg(args, nargs);

    int out_len = xr_utf16_encoded_len((const uint8_t *) str, len);
    if (out_len < 0)
        return xr_null();

    uint8_t *output = (uint8_t *) xr_malloc(out_len + 2);
    if (!output)
        return xr_null();

    int actual_len = xr_utf16_encode((const uint8_t *) str, len, output, out_len + 2, endian);
    if (actual_len < 0) {
        xr_free(output);
        return xr_null();
    }

    XrValue result = make_bytes(X, output, actual_len);
    xr_free(output);
    return result;
}

// encoding.utf16Decode(bytes, endian?, stripBom?) -> string?
//
// Handles the Unicode BOM (U+FEFF) that real-world UTF-16 files frequently
// carry as their first code unit. By default the BOM is consumed silently
// and, if the caller did not pass an explicit endian, the BOM drives the
// endian selection (FE FF = BE, FF FE = LE). Callers that wish to observe
// the BOM as a literal character can pass stripBom=false.
static XrValue encoding_utf16_decode(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1)
        return xr_null();

    const uint8_t *bytes = NULL;
    size_t len = 0;

    // Accept both string and Array<uint8>
    if (XR_IS_STRING(args[0])) {
        XrString *s = XR_TO_STRING(args[0]);
        bytes = (const uint8_t *) s->data;
        len = s->length;
    } else if (xr_value_is_array(args[0])) {
        XrArray *arr = xr_value_to_array(args[0]);
        if (arr->elem_type == XR_ELEM_U8) {
            bytes = (const uint8_t *) arr->data;
            len = arr->length;
        }
    }
    // Empty input → empty string
    if (len == 0)
        return xrs_string_value_n(X, "", 0);
    if (!bytes)
        return xr_null();

    // Auto-detect endian from BOM when the caller did not supply one.
    bool endian_explicit = (nargs >= 2 && XR_IS_INT(args[1]));
    XrUtf16Endian endian = parse_endian_arg(args, nargs);
    bool strip_bom = true;
    if (nargs >= 3 && XR_IS_BOOL(args[2])) {
        strip_bom = XR_TO_BOOL(args[2]);
    }

    if (strip_bom && len >= 2) {
        if (bytes[0] == 0xFF && bytes[1] == 0xFE) {
            if (!endian_explicit)
                endian = XR_UTF16_LE;
            bytes += 2;
            len -= 2;
        } else if (bytes[0] == 0xFE && bytes[1] == 0xFF) {
            if (!endian_explicit)
                endian = XR_UTF16_BE;
            bytes += 2;
            len -= 2;
        }
    }
    if (len == 0)
        return xrs_string_value_n(X, "", 0);

    int out_len = xr_utf16_to_utf8_len(bytes, len, endian);
    if (out_len < 0)
        return xr_null();

    uint8_t *output = (uint8_t *) xr_malloc(out_len + 1);
    if (!output)
        return xr_null();

    int actual_len = xr_utf16_decode(bytes, len, output, out_len + 1, endian);
    if (actual_len < 0) {
        xr_free(output);
        return xr_null();
    }

    XrValue result = xrs_string_value_n(X, (char *) output, actual_len);
    xr_free(output);
    return result;
}

/* ========== Module Loading ========== */

#define XR_STDLIB_VM_BIND_MODULE_ENCODING 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_ENCODING

XR_FUNC XrModule *xr_load_module_encoding(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_encoding: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "encoding");
    if (!module)
        return NULL;

    xr_stdlib_vm_bind_encoding_generated(isolate, module);

    module->loaded = true;
    return module;
}
