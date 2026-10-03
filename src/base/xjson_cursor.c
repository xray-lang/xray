/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson_cursor.c - In-place closed-schema JSON lexical operations
 *
 * KEY CONCEPT:
 *   Reads and writes charge the caller's cumulative ledger; only known members exist.
 */
#include "xjson_cursor.h"
XR_FUNC XrJsonCursor xr_json_cursor_make(char *bytes, size_t length,
    void *context, XrJsonCursorWork work) {
    bool valid = work && (bytes || !length) && length <= PTRDIFF_MAX;
    XrJsonCursor json = {bytes, bytes, valid && bytes ? bytes + length : NULL, context, work,
        valid ? XR_JSON_CURSOR_OK : XR_JSON_CURSOR_INVALID};
    return json;
}
XR_FUNC bool xr_json_cursor_fail(XrJsonCursor *json, XrJsonCursorStatus status) {
    if (json->status == XR_JSON_CURSOR_OK) json->status = status;
    return false;
}
XR_FUNC bool xr_json_cursor_work(XrJsonCursor *json, uint64_t work) {
    if (json->status != XR_JSON_CURSOR_OK) return false;
    XrJsonCursorStatus status = json->work(json->context, work);
    if (status != XR_JSON_CURSOR_OK) return xr_json_cursor_fail(json, status);
    return true;
}
XR_FUNC bool xr_json_cursor_length(XrJsonCursor *json, const char *text, size_t *output) {
    for (size_t length = 0;; ++length) {
        if (!xr_json_cursor_work(json, 1)) return false;
        if (!text[length]) { *output = length; return true; }
    }
}
XR_FUNC bool xr_json_cursor_peek(XrJsonCursor *json, uint8_t *output) {
    if (json->cursor == json->end) return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    if (!xr_json_cursor_work(json, 1)) return false;
    *output = (uint8_t)*json->cursor; return true;
}
XR_FUNC bool xr_json_cursor_byte(XrJsonCursor *json, uint8_t *output) {
    if (json->cursor == json->end) return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    if (!xr_json_cursor_work(json, 1)) return false;
    *output = (uint8_t)*json->cursor++;
    return true;
}
XR_FUNC bool xr_json_cursor_space(XrJsonCursor *json) {
    while (json->cursor != json->end) {
        uint8_t byte = 0;
        if (!xr_json_cursor_peek(json, &byte)) return false;
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') break;
        ++json->cursor;
    }
    return json->status == XR_JSON_CURSOR_OK;
}
XR_FUNC bool xr_json_cursor_take(XrJsonCursor *json, char token) {
    uint8_t actual = 0;
    return xr_json_cursor_space(json) && xr_json_cursor_byte(json, &actual) &&
        (actual == (uint8_t)token || xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID));
}
XR_FUNC bool xr_json_cursor_equal(XrJsonCursor *json, const char *left, const char *right) {
    for (;;) {
        if (!xr_json_cursor_work(json, 2)) return false;
        uint8_t a = (uint8_t)*left++, b = (uint8_t)*right++;
        if (a != b) return false;
        if (!a) return true;
    }
}
static bool xr_json_cursor_hex4(XrJsonCursor *json, uint32_t *output) {
    uint32_t code = 0;
    for (uint32_t digit = 0; digit < 4; ++digit) {
        uint8_t byte = 0;
        if (!xr_json_cursor_byte(json, &byte)) return false;
        uint32_t value = byte >= '0' && byte <= '9' ? (uint32_t)(byte - '0') :
            byte >= 'a' && byte <= 'f' ? (uint32_t)(byte - 'a' + 10) :
            byte >= 'A' && byte <= 'F' ? (uint32_t)(byte - 'A' + 10) : 16u;
        if (value == 16) return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
        code = code * 16 + value;
    }
    *output = code;
    return true;
}
static bool xr_json_cursor_unicode(XrJsonCursor *json, uint32_t *output) {
    uint32_t code = 0;
    if (!xr_json_cursor_hex4(json, &code)) return false;
    if (code >= 0xd800 && code <= 0xdbff) {
        uint8_t slash = 0, letter = 0;
        uint32_t low = 0;
        if (!xr_json_cursor_byte(json, &slash) || !xr_json_cursor_byte(json, &letter) ||
            slash != '\\' || letter != 'u' || !xr_json_cursor_hex4(json, &low) ||
            low < 0xdc00 || low > 0xdfff) return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
        code = 0x10000 + (code - 0xd800) * 0x400 + low - 0xdc00;
    } else if (code >= 0xdc00 && code <= 0xdfff) {
        return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    }
    if (!code) return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    *output = code;
    return true;
}
static size_t xr_json_cursor_encode(uint32_t code, char *output) {
    if (code < 0x80) { output[0] = (char)code; return 1; }
    if (code < 0x800) {
        output[0] = (char)(0xc0 | (code >> 6)); output[1] = (char)(0x80 | (code & 63)); return 2;
    }
    if (code < 0x10000) {
        output[0] = (char)(0xe0 | (code >> 12)); output[1] = (char)(0x80 | ((code >> 6) & 63));
        output[2] = (char)(0x80 | (code & 63)); return 3;
    }
    output[0] = (char)(0xf0 | (code >> 18)); output[1] = (char)(0x80 | ((code >> 12) & 63));
    output[2] = (char)(0x80 | ((code >> 6) & 63)); output[3] = (char)(0x80 | (code & 63)); return 4;
}
static bool xr_json_cursor_utf8(XrJsonCursor *json, uint8_t first, uint32_t *output) {
    uint32_t count = first >= 0xc2 && first <= 0xdf ? 1u :
        first >= 0xe0 && first <= 0xef ? 2u : first >= 0xf0 && first <= 0xf4 ? 3u : 0u;
    if (!count) return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    uint32_t code = first & (count == 1 ? 31u : count == 2 ? 15u : 7u);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t next = 0;
        if (!xr_json_cursor_byte(json, &next) || (next & 0xc0) != 0x80)
            return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
        code = code * 64 + (next & 63);
    }
    uint32_t minimum = count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u;
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    *output = code;
    return true;
}
XR_FUNC bool xr_json_cursor_string(XrJsonCursor *json, size_t limit, const char **output) {
    if (!xr_json_cursor_take(json, '"')) return false;
    char *begin = json->cursor, *write = begin;
    for (;;) {
        uint8_t byte = 0;
        if (!xr_json_cursor_byte(json, &byte)) return false;
        if (byte == '"') {
            if (!xr_json_cursor_work(json, 1)) return false;
            *write = 0; *output = begin; return true;
        }
        if (byte < 32) return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
        uint32_t code = byte;
        if (byte == '\\') {
            if (!xr_json_cursor_byte(json, &byte)) return false;
            if (byte == 'u') {
                if (!xr_json_cursor_unicode(json, &code)) return false;
            } else if (byte == '"' || byte == '\\' || byte == '/') code = byte;
            else if (byte == 'b') code = '\b';
            else if (byte == 'f') code = '\f';
            else if (byte == 'n') code = '\n';
            else if (byte == 'r') code = '\r';
            else if (byte == 't') code = '\t';
            else return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
        } else if (byte >= 128 && !xr_json_cursor_utf8(json, byte, &code)) return false;
        size_t bytes = code < 0x80 ? 1u : code < 0x800 ? 2u : code < 0x10000 ? 3u : 4u;
        if ((size_t)(write - begin) > limit || bytes > limit - (size_t)(write - begin))
            return xr_json_cursor_fail(json, XR_JSON_CURSOR_BUDGET);
        if (!xr_json_cursor_work(json, bytes)) return false;
        xr_json_cursor_encode(code, write);
        write += bytes;
    }
}
XR_FUNC bool xr_json_cursor_u64(XrJsonCursor *json, uint64_t *output) {
    uint8_t peek = 0;
    if (!xr_json_cursor_space(json) || !xr_json_cursor_peek(json, &peek) ||
        peek < '0' || peek > '9') return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    bool zero = peek == '0';
    uint64_t value = 0;
    uint32_t digits = 0;
    while (json->cursor != json->end) {
        if (!xr_json_cursor_peek(json, &peek)) return false;
        if (peek < '0' || peek > '9') break;
        uint8_t byte = 0;
        if (!xr_json_cursor_byte(json, &byte)) return false;
        if (zero && digits) return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
        uint32_t digit = byte - '0';
        if (value > (UINT64_MAX - digit) / 10) return xr_json_cursor_fail(json, XR_JSON_CURSOR_BUDGET);
        value = value * 10 + digit;
        ++digits;
    }
    *output = value;
    return true;
}
XR_FUNC bool xr_json_cursor_key(XrJsonCursor *json, const char *const *names, uint32_t count,
    uint64_t *seen, uint32_t *ordinal) {
    if (!names || !seen || !ordinal || count > 64)
        return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    const char *key = NULL;
    if (!xr_json_cursor_string(json, 128, &key) || !xr_json_cursor_take(json, ':')) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!xr_json_cursor_equal(json, key, names[i])) {
            if (json->status != XR_JSON_CURSOR_OK) return false;
            continue;
        }
        if (*seen & (UINT64_C(1) << i)) return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
        *seen |= UINT64_C(1) << i;
        *ordinal = i;
        return true;
    }
    return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
}
XR_FUNC bool xr_json_cursor_member_end(XrJsonCursor *json, bool *more) {
    if (!xr_json_cursor_space(json) || json->cursor == json->end)
        return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    uint8_t token = 0;
    if (!xr_json_cursor_peek(json, &token)) return false;
    if (token != ',' && token != '}') return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    if (!xr_json_cursor_take(json, (char)token)) return false;
    *more = token == ',';
    return true;
}
