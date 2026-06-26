/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_encoding_core.h - Pure encoding predicates shared by VM stdlib and AOT
 */

#ifndef XR_ENCODING_CORE_H
#define XR_ENCODING_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_ENCODING_UNICODE_MAX 0x10FFFFu
#define XR_ENCODING_UNICODE_INVALID 0xFFFDu
#define XR_ENCODING_UTF16_LE 0
#define XR_ENCODING_UTF16_BE 1

static inline int xr_encoding_core_utf16_endian_arg(bool has_int, int64_t value) {
    return has_int && value == XR_ENCODING_UTF16_BE ? XR_ENCODING_UTF16_BE : XR_ENCODING_UTF16_LE;
}

static inline bool xr_encoding_core_bool_arg_or(bool has_bool, bool value, bool fallback) {
    return has_bool ? value : fallback;
}

static inline uint8_t xr_encoding_core_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9')
        return (uint8_t) (c - '0');
    if (c >= 'a' && c <= 'f')
        return (uint8_t) (10 + c - 'a');
    if (c >= 'A' && c <= 'F')
        return (uint8_t) (10 + c - 'A');
    return 255;
}

static inline bool xr_encoding_core_hex_valid(const char *hex, size_t len) {
    if (!hex || (len % 2) != 0)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (xr_encoding_core_hex_value((unsigned char) hex[i]) == 255)
            return false;
    }
    return true;
}

static inline bool xr_encoding_core_hex_encoded_len(size_t len, size_t *out_len) {
    if (len > (SIZE_MAX - 1u) / 2u)
        return false;
    if (out_len)
        *out_len = len * 2u;
    return true;
}

static inline bool xr_encoding_core_hex_encode(const uint8_t *data, size_t len, char *output) {
    static const char chars[] = "0123456789abcdef";
    if ((!data && len != 0) || !output)
        return false;
    for (size_t i = 0; i < len; i++) {
        output[i * 2u] = chars[data[i] >> 4];
        output[i * 2u + 1u] = chars[data[i] & 0x0F];
    }
    output[len * 2u] = '\0';
    return true;
}

static inline bool xr_encoding_core_hex_decoded_len(const char *hex, size_t len, size_t *out_len) {
    if (!xr_encoding_core_hex_valid(hex, len))
        return false;
    if (out_len)
        *out_len = len / 2u;
    return true;
}

static inline bool xr_encoding_core_hex_decode(const char *hex, size_t len, uint8_t *output,
                                               size_t *out_len) {
    size_t n = 0;
    if (!xr_encoding_core_hex_decoded_len(hex, len, &n))
        return false;
    if (!output && n != 0)
        return false;
    for (size_t i = 0; i < n; i++) {
        uint8_t hi = xr_encoding_core_hex_value((unsigned char) hex[i * 2u]);
        uint8_t lo = xr_encoding_core_hex_value((unsigned char) hex[i * 2u + 1u]);
        output[i] = (uint8_t) ((hi << 4) | lo);
    }
    if (out_len)
        *out_len = n;
    return true;
}

static inline int xr_encoding_core_utf8_char_size(uint8_t first_byte) {
    if ((first_byte & 0x80) == 0x00)
        return 1;
    if ((first_byte & 0xE0) == 0xC0)
        return 2;
    if ((first_byte & 0xF0) == 0xE0)
        return 3;
    if ((first_byte & 0xF8) == 0xF0)
        return 4;
    return 1;
}

static inline bool xr_encoding_core_utf8_is_continuation(uint8_t byte) {
    return (byte & 0xC0) == 0x80;
}

static inline int xr_encoding_core_utf8_decode(const char *str, size_t len, uint32_t *out_cp) {
    if (!str || len == 0) {
        if (out_cp)
            *out_cp = 0;
        return 0;
    }

    uint8_t b0 = (uint8_t) str[0];
    uint32_t cp;
    int size;

    if ((b0 & 0x80) == 0) {
        cp = b0;
        size = 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        if (len < 2)
            goto invalid;
        uint8_t b1 = (uint8_t) str[1];
        if (!xr_encoding_core_utf8_is_continuation(b1))
            goto invalid;
        cp = ((uint32_t) (b0 & 0x1F) << 6) | (uint32_t) (b1 & 0x3F);
        if (cp < 0x80)
            goto invalid;
        size = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
        if (len < 3)
            goto invalid;
        uint8_t b1 = (uint8_t) str[1];
        uint8_t b2 = (uint8_t) str[2];
        if (!xr_encoding_core_utf8_is_continuation(b1) ||
            !xr_encoding_core_utf8_is_continuation(b2))
            goto invalid;
        cp =
            ((uint32_t) (b0 & 0x0F) << 12) | ((uint32_t) (b1 & 0x3F) << 6) | (uint32_t) (b2 & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))
            goto invalid;
        size = 3;
    } else if ((b0 & 0xF8) == 0xF0) {
        if (len < 4)
            goto invalid;
        uint8_t b1 = (uint8_t) str[1];
        uint8_t b2 = (uint8_t) str[2];
        uint8_t b3 = (uint8_t) str[3];
        if (!xr_encoding_core_utf8_is_continuation(b1) ||
            !xr_encoding_core_utf8_is_continuation(b2) ||
            !xr_encoding_core_utf8_is_continuation(b3))
            goto invalid;
        cp = ((uint32_t) (b0 & 0x07) << 18) | ((uint32_t) (b1 & 0x3F) << 12) |
             ((uint32_t) (b2 & 0x3F) << 6) | (uint32_t) (b3 & 0x3F);
        if (cp < 0x10000 || cp > XR_ENCODING_UNICODE_MAX)
            goto invalid;
        size = 4;
    } else {
        goto invalid;
    }

    if (out_cp)
        *out_cp = cp;
    return size;

invalid:
    if (out_cp)
        *out_cp = XR_ENCODING_UNICODE_INVALID;
    return 1;
}

static inline bool xr_encoding_core_utf8_valid(const char *str, size_t len) {
    if (!str)
        return false;
    size_t pos = 0;
    while (pos < len) {
        uint32_t cp;
        int size = xr_encoding_core_utf8_decode(str + pos, len - pos, &cp);
        if (size == 0 || cp == XR_ENCODING_UNICODE_INVALID)
            return false;
        pos += (size_t) size;
    }
    return true;
}

static inline size_t xr_encoding_core_utf8_count(const char *str, size_t len) {
    if (!str || len == 0)
        return 0;
    size_t count = 0;
    size_t pos = 0;
    while (pos < len) {
        int size = xr_encoding_core_utf8_char_size((uint8_t) str[pos]);
        if (pos + (size_t) size > len) {
            count++;
            break;
        }
        pos += (size_t) size;
        count++;
    }
    return count;
}

static inline size_t xr_encoding_core_utf8_byte_length(const char *str, size_t len) {
    if (!str && len != 0)
        return 0;
    return len;
}

static inline int xr_encoding_core_utf8_encode_size(uint32_t cp) {
    if (cp <= 0x7F)
        return 1;
    if (cp <= 0x7FF)
        return 2;
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0;
    if (cp <= 0xFFFF)
        return 3;
    if (cp <= XR_ENCODING_UNICODE_MAX)
        return 4;
    return 0;
}

static inline int xr_encoding_core_utf8_encode(uint32_t cp, char *output) {
    int needed = xr_encoding_core_utf8_encode_size(cp);
    if (!output || needed == 0)
        return 0;
    if (needed == 1) {
        output[0] = (char) cp;
    } else if (needed == 2) {
        output[0] = (char) (0xC0 | (cp >> 6));
        output[1] = (char) (0x80 | (cp & 0x3F));
    } else if (needed == 3) {
        output[0] = (char) (0xE0 | (cp >> 12));
        output[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        output[2] = (char) (0x80 | (cp & 0x3F));
    } else {
        output[0] = (char) (0xF0 | (cp >> 18));
        output[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
        output[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
        output[3] = (char) (0x80 | (cp & 0x3F));
    }
    return needed;
}

static inline bool xr_encoding_core_add_size(size_t *acc, size_t delta) {
    if (!acc || *acc > SIZE_MAX - delta)
        return false;
    *acc += delta;
    return true;
}

static inline bool xr_encoding_core_utf16_encoded_len(const char *utf8, size_t utf8_len,
                                                      size_t *out_len) {
    if (!utf8)
        return false;
    size_t len = 0;
    size_t pos = 0;
    while (pos < utf8_len) {
        uint32_t cp;
        int char_len = xr_encoding_core_utf8_decode(utf8 + pos, utf8_len - pos, &cp);
        if (char_len == 0 || cp == XR_ENCODING_UNICODE_INVALID)
            return false;
        if (!xr_encoding_core_add_size(&len, cp > 0xFFFF ? 4u : 2u))
            return false;
        pos += (size_t) char_len;
    }
    if (out_len)
        *out_len = len;
    return true;
}

static inline void xr_encoding_core_utf16_write_unit(uint8_t *output, size_t *pos, uint16_t unit,
                                                     int endian) {
    if (endian == XR_ENCODING_UTF16_BE) {
        output[(*pos)++] = (uint8_t) (unit >> 8);
        output[(*pos)++] = (uint8_t) (unit & 0xFF);
    } else {
        output[(*pos)++] = (uint8_t) (unit & 0xFF);
        output[(*pos)++] = (uint8_t) (unit >> 8);
    }
}

static inline bool xr_encoding_core_utf16_encode(const char *utf8, size_t utf8_len, uint8_t *output,
                                                 size_t out_cap, int endian, size_t *out_len) {
    if ((!utf8 && utf8_len != 0) || (!output && out_cap != 0))
        return false;
    size_t expected = 0;
    if (!xr_encoding_core_utf16_encoded_len(utf8, utf8_len, &expected) || expected > out_cap)
        return false;

    size_t in_pos = 0;
    size_t out_pos = 0;
    while (in_pos < utf8_len) {
        uint32_t cp;
        int char_len = xr_encoding_core_utf8_decode(utf8 + in_pos, utf8_len - in_pos, &cp);
        if (char_len == 0 || cp == XR_ENCODING_UNICODE_INVALID)
            return false;
        in_pos += (size_t) char_len;

        if (cp <= 0xFFFF) {
            xr_encoding_core_utf16_write_unit(output, &out_pos, (uint16_t) cp, endian);
        } else {
            cp -= 0x10000;
            xr_encoding_core_utf16_write_unit(output, &out_pos, (uint16_t) (0xD800u + (cp >> 10)),
                                              endian);
            xr_encoding_core_utf16_write_unit(output, &out_pos,
                                              (uint16_t) (0xDC00u + (cp & 0x3FFu)), endian);
        }
    }

    if (out_len)
        *out_len = out_pos;
    return true;
}

static inline uint16_t xr_encoding_core_utf16_read_unit(const uint8_t *data, int endian) {
    if (endian == XR_ENCODING_UTF16_BE)
        return (uint16_t) (((uint16_t) data[0] << 8) | (uint16_t) data[1]);
    return (uint16_t) ((uint16_t) data[0] | ((uint16_t) data[1] << 8));
}

typedef struct XrEncodingCoreUtf16DecodeView {
    const uint8_t *data;
    size_t len;
    int endian;
} XrEncodingCoreUtf16DecodeView;

static inline XrEncodingCoreUtf16DecodeView
xr_encoding_core_utf16_decode_view(const uint8_t *data, size_t len, int endian,
                                   bool endian_explicit, bool strip_bom) {
    XrEncodingCoreUtf16DecodeView view = {
        .data = data,
        .len = len,
        .endian = endian,
    };
    if (!strip_bom || !data || len < 2)
        return view;

    if (data[0] == 0xFF && data[1] == 0xFE) {
        if (!endian_explicit)
            view.endian = XR_ENCODING_UTF16_LE;
        view.data = data + 2;
        view.len = len - 2;
    } else if (data[0] == 0xFE && data[1] == 0xFF) {
        if (!endian_explicit)
            view.endian = XR_ENCODING_UTF16_BE;
        view.data = data + 2;
        view.len = len - 2;
    }
    return view;
}

static inline bool xr_encoding_core_utf16_to_utf8_len(const uint8_t *utf16, size_t utf16_len,
                                                      int endian, size_t *out_len) {
    if ((!utf16 && utf16_len != 0) || (utf16_len % 2u) != 0)
        return false;
    size_t len = 0;
    size_t pos = 0;
    while (pos < utf16_len) {
        uint16_t unit = xr_encoding_core_utf16_read_unit(utf16 + pos, endian);
        pos += 2u;
        uint32_t cp;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (pos + 2u > utf16_len)
                return false;
            uint16_t low = xr_encoding_core_utf16_read_unit(utf16 + pos, endian);
            if (low < 0xDC00 || low > 0xDFFF)
                return false;
            pos += 2u;
            cp = 0x10000u + (((uint32_t) unit - 0xD800u) << 10) + ((uint32_t) low - 0xDC00u);
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;
        } else {
            cp = unit;
        }
        int needed = xr_encoding_core_utf8_encode_size(cp);
        if (needed == 0 || !xr_encoding_core_add_size(&len, (size_t) needed))
            return false;
    }
    if (out_len)
        *out_len = len;
    return true;
}

static inline bool xr_encoding_core_utf16_decode(const uint8_t *utf16, size_t utf16_len,
                                                 char *output, size_t out_cap, int endian,
                                                 size_t *out_len) {
    if ((!utf16 && utf16_len != 0) || (!output && out_cap != 0))
        return false;
    size_t expected = 0;
    if (!xr_encoding_core_utf16_to_utf8_len(utf16, utf16_len, endian, &expected) ||
        expected > out_cap)
        return false;

    size_t in_pos = 0;
    size_t out_pos = 0;
    while (in_pos < utf16_len) {
        uint16_t unit = xr_encoding_core_utf16_read_unit(utf16 + in_pos, endian);
        in_pos += 2u;
        uint32_t cp;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            uint16_t low = xr_encoding_core_utf16_read_unit(utf16 + in_pos, endian);
            in_pos += 2u;
            cp = 0x10000u + (((uint32_t) unit - 0xD800u) << 10) + ((uint32_t) low - 0xDC00u);
        } else {
            cp = unit;
        }
        int written = xr_encoding_core_utf8_encode(cp, output + out_pos);
        if (written == 0)
            return false;
        out_pos += (size_t) written;
    }

    if (out_len)
        *out_len = out_pos;
    return true;
}

#endif  // XR_ENCODING_CORE_H
