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
 *   Hex encoding uses lookup tables from xray_simd.h.
 *   UTF-8 operations delegate to core xutf8.h (zero duplication).
 *   UTF-16 encode returns Array<uint8> (binary data, not string).
 */

#include "encoding.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/base/xmalloc.h"
#include "../../src/runtime/gc/xgc.h"
#include <string.h>

/* ========== Hex Encoding ========== */

static const char HEX_CHARS_LOWER[] = "0123456789abcdef";

int xr_hex_encode(const uint8_t *data, size_t len, char *output) {
    if (!data || !output) return 0;

    for (size_t i = 0; i < len; i++) {
        output[i * 2] = HEX_CHARS_LOWER[data[i] >> 4];
        output[i * 2 + 1] = HEX_CHARS_LOWER[data[i] & 0x0F];
    }
    output[len * 2] = '\0';
    return (int)(len * 2);
}

int xr_hex_decode(const char *hex, size_t len, uint8_t *output) {
    if (!hex || !output) return -1;
    if (len % 2 != 0) return -1;

    size_t out_len = len / 2;
    for (size_t i = 0; i < out_len; i++) {
        uint8_t hi = XR_HEX_TO_VAL(hex[i * 2]);
        uint8_t lo = XR_HEX_TO_VAL(hex[i * 2 + 1]);
        if (hi == 255 || lo == 255) return -1;
        output[i] = (hi << 4) | lo;
    }
    return (int)out_len;
}

bool xr_hex_valid(const char *hex, size_t len) {
    if (!hex) return false;
    if (len % 2 != 0) return false;

    for (size_t i = 0; i < len; i++) {
        if (XR_HEX_TO_VAL(hex[i]) == 255) return false;
    }
    return true;
}

/* ========== UTF-16 Implementation ========== */

int xr_utf16_encode(const uint8_t *utf8, size_t utf8_len,
                    uint8_t *output, size_t out_cap, XrUtf16Endian endian) {
    if (!utf8 || !output) return -1;

    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < utf8_len) {
        uint32_t cp;
        int char_len = xr_utf8_decode((const char*)(utf8 + in_pos), utf8_len - in_pos, &cp);
        if (char_len == 0) return -1;
        if (cp == XR_UNICODE_INVALID) return -1;
        in_pos += char_len;

        if (cp <= 0xFFFF) {
            if (out_pos + 2 > out_cap) return -1;
            if (endian == XR_UTF16_LE) {
                output[out_pos++] = cp & 0xFF;
                output[out_pos++] = (cp >> 8) & 0xFF;
            } else {
                output[out_pos++] = (cp >> 8) & 0xFF;
                output[out_pos++] = cp & 0xFF;
            }
        } else {
            if (out_pos + 4 > out_cap) return -1;
            cp -= 0x10000;
            uint16_t high = 0xD800 + (cp >> 10);
            uint16_t low = 0xDC00 + (cp & 0x3FF);
            if (endian == XR_UTF16_LE) {
                output[out_pos++] = high & 0xFF;
                output[out_pos++] = (high >> 8) & 0xFF;
                output[out_pos++] = low & 0xFF;
                output[out_pos++] = (low >> 8) & 0xFF;
            } else {
                output[out_pos++] = (high >> 8) & 0xFF;
                output[out_pos++] = high & 0xFF;
                output[out_pos++] = (low >> 8) & 0xFF;
                output[out_pos++] = low & 0xFF;
            }
        }
    }

    return (int)out_pos;
}

int xr_utf16_decode(const uint8_t *utf16, size_t utf16_len,
                    uint8_t *output, size_t out_cap, XrUtf16Endian endian) {
    if (!utf16 || !output) return -1;
    if (utf16_len % 2 != 0) return -1;

    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < utf16_len) {
        uint16_t unit;
        if (endian == XR_UTF16_LE) {
            unit = utf16[in_pos] | (utf16[in_pos + 1] << 8);
        } else {
            unit = (utf16[in_pos] << 8) | utf16[in_pos + 1];
        }
        in_pos += 2;

        uint32_t cp;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (in_pos + 2 > utf16_len) return -1;
            uint16_t low;
            if (endian == XR_UTF16_LE) {
                low = utf16[in_pos] | (utf16[in_pos + 1] << 8);
            } else {
                low = (utf16[in_pos] << 8) | utf16[in_pos + 1];
            }
            if (low < 0xDC00 || low > 0xDFFF) return -1;
            in_pos += 2;
            cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return -1;
        } else {
            cp = unit;
        }

        int needed = xr_utf8_encode_size(cp);
        if (needed == 0) return -1;
        if (out_pos + needed > out_cap) return -1;
        int written = xr_utf8_encode(cp, (char*)(output + out_pos));
        if (written == 0) return -1;
        out_pos += written;
    }

    return (int)out_pos;
}

int xr_utf16_encoded_len(const uint8_t *utf8, size_t utf8_len) {
    if (!utf8) return 0;

    int len = 0;
    size_t i = 0;
    while (i < utf8_len) {
        uint32_t cp;
        int char_len = xr_utf8_decode((const char*)(utf8 + i), utf8_len - i, &cp);
        if (char_len == 0 || cp == XR_UNICODE_INVALID) return -1;
        i += char_len;
        len += (cp > 0xFFFF) ? 4 : 2;
    }
    return len;
}

int xr_utf8_decoded_len(const uint8_t *utf16, size_t utf16_len, XrUtf16Endian endian) {
    if (!utf16 || utf16_len % 2 != 0) return -1;

    int len = 0;
    size_t i = 0;
    while (i < utf16_len) {
        uint16_t unit;
        if (endian == XR_UTF16_LE) {
            unit = utf16[i] | (utf16[i + 1] << 8);
        } else {
            unit = (utf16[i] << 8) | utf16[i + 1];
        }
        i += 2;

        uint32_t cp;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 2 > utf16_len) return -1;
            i += 2;
            cp = 0x10000;
        } else {
            cp = unit;
        }

        if (cp <= 0x7F) len += 1;
        else if (cp <= 0x7FF) len += 2;
        else if (cp <= 0xFFFF) len += 3;
        else len += 4;
    }
    return len;
}

/* ========== Helper Functions ========== */

static const char* get_string_arg(XrValue v, size_t *len) {
    if (!XR_IS_STRING(v)) return NULL;
    XrString *str = XR_TO_STRING(v);
    if (len) *len = str->length;
    return str->data;
}

static XrValue make_string(XrayIsolate *X, const char *s, size_t len) {
    if (!s) return xr_null();
    XrString *str = xr_string_new(X, s, len);
    return xr_string_value(str);
}

static XrValue make_bytes(XrayIsolate *X, const uint8_t *data, int len) {
    XrCoroutine *coro = xr_current_coro(X);
    if (!coro) return xr_null();
    XrArray *arr = xr_array_with_capacity_typed(coro, len, XR_ELEM_U8);
    if (!arr) return xr_null();
    if (len > 0) {
        memcpy(arr->data, data, len);
        arr->length = (int32_t)len;
    }
    return xr_value_from_array(arr);
}

/* ========== xray Binding Functions ========== */

// encoding.hexEncode(str) -> string
static XrValue encoding_hex_encode(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return xr_null();

    size_t len;
    const char *data = get_string_arg(args[0], &len);
    if (!data) return xr_null();

    char *output = (char*)xr_malloc(len * 2 + 1);
    if (!output) return xr_null();

    xr_hex_encode((const uint8_t*)data, len, output);
    XrValue result = make_string(X, output, len * 2);
    xr_free(output);
    return result;
}

// encoding.hexDecode(hex) -> Array<uint8>
static XrValue encoding_hex_decode(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return xr_null();

    size_t len;
    const char *hex = get_string_arg(args[0], &len);
    if (!hex) return xr_null();

    uint8_t *output = (uint8_t*)xr_malloc(len / 2 + 1);
    if (!output) return xr_null();

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
static XrValue encoding_hex_decode_string(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return xr_null();

    size_t len;
    const char *hex = get_string_arg(args[0], &len);
    if (!hex) return xr_null();

    uint8_t *output = (uint8_t*)xr_malloc(len / 2 + 1);
    if (!output) return xr_null();

    int out_len = xr_hex_decode(hex, len, output);
    if (out_len < 0) {
        xr_free(output);
        return xr_null();
    }

    XrValue result = make_string(X, (char*)output, out_len);
    xr_free(output);
    return result;
}

// encoding.hexValid(hex) -> bool
static XrValue encoding_hex_valid(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_bool(false);

    size_t len;
    const char *hex = get_string_arg(args[0], &len);
    if (!hex) return xr_bool(false);

    return xr_bool(xr_hex_valid(hex, len));
}

// encoding.utf8Valid(str) -> bool
static XrValue encoding_utf8_valid(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_bool(false);

    size_t len;
    const char *str = get_string_arg(args[0], &len);
    if (!str) return xr_bool(false);

    return xr_bool(xr_utf8_validate(str, len));
}

// encoding.utf8Count(str) -> int
static XrValue encoding_utf8_count(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_int(0);

    size_t len;
    const char *str = get_string_arg(args[0], &len);
    if (!str) return xr_int(0);

    return xr_int((int64_t)xr_utf8_strlen(str, len));
}

// encoding.utf8ByteLength(str) -> int
static XrValue encoding_utf8_byte_length(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_int(0);

    size_t len;
    const char *str = get_string_arg(args[0], &len);
    if (!str) return xr_int(0);

    return xr_int((int64_t)len);
}

static XrUtf16Endian parse_endian_arg(XrValue *args, int nargs) {
    if (nargs >= 2 && XR_IS_INT(args[1])) {
        return XR_TO_INT(args[1]) == XR_UTF16_BE ? XR_UTF16_BE : XR_UTF16_LE;
    }
    return XR_UTF16_LE;
}

// encoding.utf16Encode(str, endian?) -> Array<uint8>
static XrValue encoding_utf16_encode(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return xr_null();

    size_t len;
    const char *str = get_string_arg(args[0], &len);
    if (!str) return xr_null();

    XrUtf16Endian endian = parse_endian_arg(args, nargs);

    int out_len = xr_utf16_encoded_len((const uint8_t*)str, len);
    if (out_len < 0) return xr_null();

    uint8_t *output = (uint8_t*)xr_malloc(out_len + 2);
    if (!output) return xr_null();

    int actual_len = xr_utf16_encode((const uint8_t*)str, len, output, out_len + 2, endian);
    if (actual_len < 0) {
        xr_free(output);
        return xr_null();
    }

    XrValue result = make_bytes(X, output, actual_len);
    xr_free(output);
    return result;
}

// encoding.utf16Decode(bytes, endian?) -> string?
static XrValue encoding_utf16_decode(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return xr_null();

    const uint8_t *bytes = NULL;
    size_t len = 0;

    // Accept both string and Array<uint8>
    if (XR_IS_STRING(args[0])) {
        XrString *s = XR_TO_STRING(args[0]);
        bytes = (const uint8_t*)s->data;
        len = s->length;
    } else if (xr_value_is_array(args[0])) {
        XrArray *arr = xr_value_to_array(args[0]);
        if (arr->elem_type == XR_ELEM_U8) {
            bytes = (const uint8_t*)arr->data;
            len = arr->length;
        }
    }
    // Empty input → empty string
    if (len == 0) return make_string(X, "", 0);
    if (!bytes) return xr_null();

    XrUtf16Endian endian = parse_endian_arg(args, nargs);

    int out_len = xr_utf8_decoded_len(bytes, len, endian);
    if (out_len < 0) return xr_null();

    uint8_t *output = (uint8_t*)xr_malloc(out_len + 1);
    if (!output) return xr_null();

    int actual_len = xr_utf16_decode(bytes, len, output, out_len + 1, endian);
    if (actual_len < 0) {
        xr_free(output);
        return xr_null();
    }

    XrValue result = make_string(X, (char*)output, actual_len);
    xr_free(output);
    return result;
}

/* ========== Module Loading ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module encoding

XR_DEFINE_BUILTIN(encoding_hex_encode, "hexEncode", "(data: string): string", "Hex encode string to hex")
XR_DEFINE_BUILTIN(encoding_hex_decode, "hexDecode", "(hex: string): Array<uint8>?", "Hex decode to bytes")
XR_DEFINE_BUILTIN(encoding_hex_decode_string, "hexDecodeString", "(hex: string): string?", "Hex decode to string")
XR_DEFINE_BUILTIN(encoding_hex_valid, "hexValid", "(hex: string): bool", "Check if valid hex string")
XR_DEFINE_BUILTIN(encoding_utf8_valid, "utf8Valid", "(data: string): bool", "Check if valid UTF-8")
XR_DEFINE_BUILTIN(encoding_utf8_count, "utf8Count", "(data: string): int", "Count UTF-8 characters")
XR_DEFINE_BUILTIN(encoding_utf8_byte_length, "utf8ByteLength", "(data: string): int", "Get UTF-8 byte length")
XR_DEFINE_BUILTIN(encoding_utf16_encode, "utf16Encode", "(data: string, endian?: int): Array<uint8>", "UTF-16 encode to bytes")
XR_DEFINE_BUILTIN(encoding_utf16_decode, "utf16Decode", "(data: any, endian?: int): string?", "UTF-16 decode to string")

XrModule* xr_load_module_encoding(XrayIsolate *isolate) {
    XrModule *module = xr_module_create_native(isolate, "encoding");
    if (!module) return NULL;

    #define EXPORT_CFUNC(name_str, func_ptr) \
        do { \
            XrCFunction *cfunc = xr_vm_cfunction_new(isolate, func_ptr, name_str); \
            XrValue fn_val = xr_value_from_cfunction(cfunc); \
            xr_module_add_export(isolate, module, name_str, fn_val); \
        } while(0)

    // Hex encoding
    EXPORT_CFUNC("hexEncode", encoding_hex_encode);
    EXPORT_CFUNC("hexDecode", encoding_hex_decode);
    EXPORT_CFUNC("hexDecodeString", encoding_hex_decode_string);
    EXPORT_CFUNC("hexValid", encoding_hex_valid);

    // UTF-8 operations
    EXPORT_CFUNC("utf8Valid", encoding_utf8_valid);
    EXPORT_CFUNC("utf8Count", encoding_utf8_count);
    EXPORT_CFUNC("utf8ByteLength", encoding_utf8_byte_length);

    // UTF-16 encoding
    EXPORT_CFUNC("utf16Encode", encoding_utf16_encode);
    EXPORT_CFUNC("utf16Decode", encoding_utf16_decode);

    // Endian constants
    xr_module_add_export(isolate, module, "LE", xr_int(XR_UTF16_LE));
    xr_module_add_export(isolate, module, "BE", xr_int(XR_UTF16_BE));

    #undef EXPORT_CFUNC

    module->loaded = true;
    return module;
}
