/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_string_core.h - Runtime-neutral string helper rules shared by VM and AOT
 */

#ifndef XRAY_SHARED_XR_STRING_CORE_H
#define XRAY_SHARED_XR_STRING_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef enum XrStringCoreTrimMode {
    XR_STRING_CORE_TRIM_BOTH = 0,
    XR_STRING_CORE_TRIM_START,
    XR_STRING_CORE_TRIM_END
} XrStringCoreTrimMode;

typedef struct XrStringCoreSlice {
    const char *data;
    size_t len;
} XrStringCoreSlice;

typedef enum XrStringCoreRepeatKind {
    XR_STRING_CORE_REPEAT_INVALID = 0,
    XR_STRING_CORE_REPEAT_EMPTY,
    XR_STRING_CORE_REPEAT_ORIGINAL,
    XR_STRING_CORE_REPEAT_ALLOC
} XrStringCoreRepeatKind;

typedef struct XrStringCoreRepeatPlan {
    XrStringCoreRepeatKind kind;
    size_t len;
} XrStringCoreRepeatPlan;

typedef enum XrStringCorePadKind {
    XR_STRING_CORE_PAD_INVALID = 0,
    XR_STRING_CORE_PAD_ORIGINAL,
    XR_STRING_CORE_PAD_ALLOC
} XrStringCorePadKind;

typedef enum XrStringCorePadSide {
    XR_STRING_CORE_PAD_START = 0,
    XR_STRING_CORE_PAD_END
} XrStringCorePadSide;

typedef struct XrStringCorePadPlan {
    XrStringCorePadKind kind;
    size_t len;
    size_t fill_len;
    const char *pad;
    size_t pad_len;
} XrStringCorePadPlan;

typedef struct XrStringCoreParseIntResult {
    bool ok;
    int64_t value;
} XrStringCoreParseIntResult;

typedef struct XrStringCoreParseFloatResult {
    bool ok;
    double value;
} XrStringCoreParseFloatResult;

#define XR_STRING_CORE_UNICODE_MAX UINT32_C(0x10FFFF)
#define XR_STRING_CORE_UNICODE_INVALID UINT32_C(0xFFFD)

static inline int64_t xr_string_core_len_i64(size_t len) {
    return len > (size_t) INT64_MAX ? INT64_MAX : (int64_t) len;
}

static inline XrStringCoreSlice xr_string_core_slice_at(const char *data, size_t len, size_t start,
                                                        size_t slice_len) {
    XrStringCoreSlice out = {data, 0};
    if (!data && len != 0)
        return out;
    if (start > len)
        start = len;
    if (slice_len > len - start)
        slice_len = len - start;
    out.data = data ? data + start : data;
    out.len = slice_len;
    return out;
}

static inline XrStringCoreRepeatPlan xr_string_core_repeat_plan(const char *data, size_t len,
                                                                int64_t count) {
    XrStringCoreRepeatPlan out = {XR_STRING_CORE_REPEAT_INVALID, 0};
    if (!data && len != 0)
        return out;
    if (count <= 0) {
        out.kind = XR_STRING_CORE_REPEAT_EMPTY;
        return out;
    }
    if (count == 1) {
        out.kind = XR_STRING_CORE_REPEAT_ORIGINAL;
        out.len = len;
        return out;
    }
    if (len == 0) {
        out.kind = XR_STRING_CORE_REPEAT_EMPTY;
        return out;
    }

    uint64_t n64 = (uint64_t) count;
    if (n64 > (uint64_t) SIZE_MAX)
        return out;
    size_t n = (size_t) n64;
    if (len > SIZE_MAX / n)
        return out;

    out.kind = XR_STRING_CORE_REPEAT_ALLOC;
    out.len = len * n;
    return out;
}

static inline size_t xr_string_core_repeat_write(char *out, const char *data, size_t len,
                                                 int64_t count) {
    if (!out)
        return 0;

    XrStringCoreRepeatPlan plan = xr_string_core_repeat_plan(data, len, count);
    if (plan.kind == XR_STRING_CORE_REPEAT_INVALID)
        return 0;
    if (plan.kind == XR_STRING_CORE_REPEAT_EMPTY) {
        out[0] = '\0';
        return 0;
    }
    if (plan.len == 0) {
        out[0] = '\0';
        return 0;
    }

    memcpy(out, data, len);
    size_t written = len;
    while (written < plan.len) {
        size_t copy_len = written;
        if (copy_len > plan.len - written)
            copy_len = plan.len - written;
        memcpy(out + written, out, copy_len);
        written += copy_len;
    }
    out[plan.len] = '\0';
    return plan.len;
}

static inline XrStringCorePadPlan xr_string_core_pad_plan(const char *data, size_t len,
                                                          int64_t target_len, const char *pad_data,
                                                          size_t pad_len) {
    XrStringCorePadPlan out = {XR_STRING_CORE_PAD_INVALID, 0, 0, NULL, 0};
    if (!data && len != 0)
        return out;
    if (!pad_data && pad_len != 0)
        return out;

    int64_t n = xr_string_core_len_i64(len);
    if (target_len <= n) {
        out.kind = XR_STRING_CORE_PAD_ORIGINAL;
        out.len = len;
        return out;
    }

    uint64_t target64 = (uint64_t) target_len;
    if (target64 > (uint64_t) SIZE_MAX)
        return out;

    if (!pad_data) {
        pad_data = " ";
        pad_len = 1;
    } else if (pad_len == 0) {
        out.kind = XR_STRING_CORE_PAD_ORIGINAL;
        out.len = len;
        return out;
    }

    size_t target = (size_t) target64;
    out.kind = XR_STRING_CORE_PAD_ALLOC;
    out.len = target;
    out.fill_len = target - len;
    out.pad = pad_data;
    out.pad_len = pad_len;
    return out;
}

static inline void xr_string_core_pad_fill(char *out, size_t fill_len, const char *pad,
                                           size_t pad_len) {
    if (!out || !pad || pad_len == 0)
        return;
    size_t pos = 0;
    while (pos < fill_len) {
        size_t copy_len = pad_len;
        if (copy_len > fill_len - pos)
            copy_len = fill_len - pos;
        memcpy(out + pos, pad, copy_len);
        pos += copy_len;
    }
}

static inline size_t xr_string_core_pad_write(char *out, const char *data, size_t len,
                                              XrStringCorePadPlan plan, XrStringCorePadSide side) {
    if (!out)
        return 0;
    if (plan.kind == XR_STRING_CORE_PAD_INVALID)
        return 0;
    if (plan.kind == XR_STRING_CORE_PAD_ORIGINAL) {
        if (len != 0)
            memcpy(out, data, len);
        out[len] = '\0';
        return len;
    }

    if (side == XR_STRING_CORE_PAD_START) {
        xr_string_core_pad_fill(out, plan.fill_len, plan.pad, plan.pad_len);
        if (len != 0)
            memcpy(out + plan.fill_len, data, len);
    } else {
        if (len != 0)
            memcpy(out, data, len);
        xr_string_core_pad_fill(out + len, plan.fill_len, plan.pad, plan.pad_len);
    }
    out[plan.len] = '\0';
    return plan.len;
}

static inline int xr_string_core_utf8_char_size(unsigned char first_byte) {
    if ((first_byte & 0x80u) == 0x00u)
        return 1;
    if ((first_byte & 0xE0u) == 0xC0u)
        return 2;
    if ((first_byte & 0xF0u) == 0xE0u)
        return 3;
    if ((first_byte & 0xF8u) == 0xF0u)
        return 4;
    return 1;
}

static inline bool xr_string_core_utf8_is_continuation(unsigned char byte) {
    return (byte & 0xC0u) == 0x80u;
}

static inline int xr_string_core_utf8_decode(const char *data, size_t len, uint32_t *out_cp) {
    if (!data || len == 0) {
        if (out_cp)
            *out_cp = 0;
        return 0;
    }

    const unsigned char *s = (const unsigned char *) data;
    uint32_t cp = 0;
    int size = 0;

    if ((s[0] & 0x80u) == 0) {
        cp = s[0];
        size = 1;
    } else if ((s[0] & 0xE0u) == 0xC0u) {
        if (len < 2 || !xr_string_core_utf8_is_continuation(s[1]))
            goto invalid;
        cp = ((uint32_t) (s[0] & 0x1Fu) << 6) | (uint32_t) (s[1] & 0x3Fu);
        if (cp < 0x80u)
            goto invalid;
        size = 2;
    } else if ((s[0] & 0xF0u) == 0xE0u) {
        if (len < 3 || !xr_string_core_utf8_is_continuation(s[1]) ||
            !xr_string_core_utf8_is_continuation(s[2]))
            goto invalid;
        cp = ((uint32_t) (s[0] & 0x0Fu) << 12) | ((uint32_t) (s[1] & 0x3Fu) << 6) |
             (uint32_t) (s[2] & 0x3Fu);
        if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))
            goto invalid;
        size = 3;
    } else if ((s[0] & 0xF8u) == 0xF0u) {
        if (len < 4 || !xr_string_core_utf8_is_continuation(s[1]) ||
            !xr_string_core_utf8_is_continuation(s[2]) ||
            !xr_string_core_utf8_is_continuation(s[3]))
            goto invalid;
        cp = ((uint32_t) (s[0] & 0x07u) << 18) | ((uint32_t) (s[1] & 0x3Fu) << 12) |
             ((uint32_t) (s[2] & 0x3Fu) << 6) | (uint32_t) (s[3] & 0x3Fu);
        if (cp < 0x10000u || cp > XR_STRING_CORE_UNICODE_MAX)
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
        *out_cp = XR_STRING_CORE_UNICODE_INVALID;
    return 1;
}

static inline size_t xr_string_core_utf8_char_count(const char *data, size_t len) {
    if (!data || len == 0)
        return 0;

    size_t count = 0;
    size_t pos = 0;
    while (pos < len) {
        int size = xr_string_core_utf8_char_size((unsigned char) data[pos]);
        if (pos + (size_t) size > len) {
            count++;
            break;
        }
        pos += (size_t) size;
        count++;
    }
    return count;
}

static inline bool xr_string_core_utf8_char_at(const char *data, size_t len, size_t index,
                                               uint32_t *out_cp, size_t *out_pos) {
    if (!data)
        return false;

    size_t pos = 0;
    size_t char_idx = 0;
    while (pos < len && char_idx < index) {
        int size = xr_string_core_utf8_char_size((unsigned char) data[pos]);
        if (pos + (size_t) size > len)
            break;
        pos += (size_t) size;
        char_idx++;
    }

    if (char_idx != index || pos >= len)
        return false;

    if (out_pos)
        *out_pos = pos;
    if (out_cp)
        xr_string_core_utf8_decode(data + pos, len - pos, out_cp);
    return true;
}

static inline XrStringCoreSlice xr_string_core_utf8_char_slice_at(const char *data, size_t len,
                                                                  int64_t index) {
    if (!data || len == 0)
        return xr_string_core_slice_at(data, len, len, 0);

    if (index < 0) {
        int64_t char_len = xr_string_core_len_i64(xr_string_core_utf8_char_count(data, len));
        index += char_len;
        if (index < 0)
            return xr_string_core_slice_at(data, len, len, 0);
    }

    size_t pos = 0;
    if (!xr_string_core_utf8_char_at(data, len, (size_t) index, NULL, &pos))
        return xr_string_core_slice_at(data, len, len, 0);

    int size = xr_string_core_utf8_char_size((unsigned char) data[pos]);
    if (pos + (size_t) size > len)
        return xr_string_core_slice_at(data, len, len, 0);
    return xr_string_core_slice_at(data, len, pos, (size_t) size);
}

static inline XrStringCoreSlice xr_string_core_byte_slice_at(const char *data, size_t len,
                                                             int64_t index) {
    if (!data || len == 0)
        return xr_string_core_slice_at(data, len, len, 0);

    int64_t n = xr_string_core_len_i64(len);
    if (index < 0)
        index += n;
    if (index < 0 || index >= n)
        return xr_string_core_slice_at(data, len, len, 0);
    return xr_string_core_slice_at(data, len, (size_t) index, 1);
}

static inline bool xr_string_core_codepoint_at(const char *data, size_t len, int64_t index,
                                               uint32_t *out_cp) {
    if (index < 0)
        return false;
    return xr_string_core_utf8_char_at(data, len, (size_t) index, out_cp, NULL);
}

static inline XrStringCoreSlice xr_string_core_substring_slice(const char *data, size_t len,
                                                               int64_t start, int64_t end) {
    int64_t n = xr_string_core_len_i64(len);
    if (start < 0)
        start = 0;
    if (end < 0 || end > n)
        end = n;
    if (start >= end || start >= n)
        return xr_string_core_slice_at(data, len, len, 0);
    return xr_string_core_slice_at(data, len, (size_t) start, (size_t) (end - start));
}

static inline XrStringCoreSlice xr_string_core_range_slice(const char *data, size_t len,
                                                           int64_t start, int64_t end) {
    int64_t n = xr_string_core_len_i64(len);
    if (start < 0) {
        start += n;
        if (start < 0)
            start = 0;
    }
    if (end < 0) {
        end += n;
        if (end < 0)
            end = 0;
    }
    if (start > n)
        start = n;
    if (end > n)
        end = n;
    if (start >= end)
        return xr_string_core_slice_at(data, len, (size_t) start, 0);
    return xr_string_core_slice_at(data, len, (size_t) start, (size_t) (end - start));
}

static inline bool xr_string_core_is_ascii_whitespace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static inline const char *xr_string_core_skip_number_leading_ws(const char *data, size_t len) {
    if (!data)
        return NULL;
    size_t pos = 0;
    while (pos < len && xr_string_core_is_ascii_whitespace((unsigned char) data[pos]))
        pos++;
    return data + pos;
}

static inline XrStringCoreParseIntResult xr_string_core_parse_int64(const char *data, size_t len) {
    XrStringCoreParseIntResult out = {false, 0};
    if (!data)
        return out;

    const char *p = xr_string_core_skip_number_leading_ws(data, len);
    if (!p || p >= data + len)
        return out;

    char stack_buf[128];
    size_t parse_len = (size_t) ((data + len) - p);
    char *buf = stack_buf;
    if (parse_len >= sizeof(stack_buf)) {
        buf = (char *) malloc(parse_len + 1);
        if (!buf)
            return out;
    }
    memcpy(buf, p, parse_len);
    buf[parse_len] = '\0';

    char *end = NULL;
    long long value = strtoll(buf, &end, 10);
    bool ok = end != buf;
    if (buf != stack_buf)
        free(buf);
    if (!ok)
        return out;

    out.ok = true;
    out.value = (int64_t) value;
    return out;
}

static inline XrStringCoreParseFloatResult xr_string_core_parse_float64(const char *data,
                                                                        size_t len) {
    XrStringCoreParseFloatResult out = {false, 0.0};
    if (!data)
        return out;

    const char *p = xr_string_core_skip_number_leading_ws(data, len);
    if (!p || p >= data + len)
        return out;

    char stack_buf[128];
    size_t parse_len = (size_t) ((data + len) - p);
    char *buf = stack_buf;
    if (parse_len >= sizeof(stack_buf)) {
        buf = (char *) malloc(parse_len + 1);
        if (!buf)
            return out;
    }
    memcpy(buf, p, parse_len);
    buf[parse_len] = '\0';

    char *end = NULL;
    double value = strtod(buf, &end);
    bool ok = end != buf;
    if (buf != stack_buf)
        free(buf);
    if (!ok)
        return out;

    out.ok = true;
    out.value = value;
    return out;
}

static inline XrStringCoreSlice xr_string_core_trim_slice(const char *data, size_t len,
                                                          XrStringCoreTrimMode mode) {
    XrStringCoreSlice out = {data, len};
    if ((!data && len != 0) || len == 0)
        return out;

    size_t start = 0;
    size_t end = len;
    if (mode != XR_STRING_CORE_TRIM_END) {
        while (start < end && xr_string_core_is_ascii_whitespace((unsigned char) data[start]))
            start++;
    }
    if (mode != XR_STRING_CORE_TRIM_START) {
        while (end > start && xr_string_core_is_ascii_whitespace((unsigned char) data[end - 1]))
            end--;
    }
    out.data = data + start;
    out.len = end - start;
    return out;
}

static inline ptrdiff_t xr_string_core_index_of(const char *haystack, size_t haystack_len,
                                                const char *needle, size_t needle_len) {
    if ((!haystack && haystack_len != 0) || (!needle && needle_len != 0))
        return -1;
    if (needle_len == 0)
        return 0;
    if (needle_len > haystack_len)
        return -1;

    if (needle_len == 1) {
        const char *p = (const char *) memchr(haystack, needle[0], haystack_len);
        return p ? (ptrdiff_t) (p - haystack) : -1;
    }

    if (needle_len <= 8) {
        char first = needle[0];
        size_t limit = haystack_len - needle_len;
        for (size_t i = 0; i <= limit;) {
            const char *p = (const char *) memchr(haystack + i, first, limit - i + 1);
            if (!p)
                return -1;
            i = (size_t) (p - haystack);
            if (memcmp(p, needle, needle_len) == 0)
                return (ptrdiff_t) i;
            i++;
        }
        return -1;
    }

    size_t skip[256];
    for (int c = 0; c < 256; c++)
        skip[c] = needle_len;
    for (size_t i = 0; i < needle_len - 1; i++)
        skip[(unsigned char) needle[i]] = needle_len - 1 - i;

    size_t i = 0;
    size_t limit = haystack_len - needle_len;
    while (i <= limit) {
        size_t j = needle_len - 1;
        while (j > 0 && haystack[i + j] == needle[j])
            j--;
        if (j == 0 && haystack[i] == needle[0])
            return (ptrdiff_t) i;
        i += skip[(unsigned char) haystack[i + needle_len - 1]];
    }
    return -1;
}

static inline ptrdiff_t xr_string_core_last_index_of(const char *haystack, size_t haystack_len,
                                                     const char *needle, size_t needle_len) {
    if ((!haystack && haystack_len != 0) || (!needle && needle_len != 0))
        return -1;
    if (needle_len == 0)
        return (ptrdiff_t) haystack_len;
    if (needle_len > haystack_len)
        return -1;

    size_t last_pos = haystack_len - needle_len;
    for (size_t i = last_pos + 1; i > 0; i--) {
        size_t pos = i - 1;
        if (memcmp(haystack + pos, needle, needle_len) == 0)
            return (ptrdiff_t) pos;
    }
    return -1;
}

static inline bool xr_string_core_contains(const char *haystack, size_t haystack_len,
                                           const char *needle, size_t needle_len) {
    return xr_string_core_index_of(haystack, haystack_len, needle, needle_len) >= 0;
}

static inline bool xr_string_core_starts_with(const char *haystack, size_t haystack_len,
                                              const char *prefix, size_t prefix_len) {
    if ((!haystack && haystack_len != 0) || (!prefix && prefix_len != 0))
        return false;
    if (prefix_len > haystack_len)
        return false;
    if (prefix_len == 0)
        return true;
    return memcmp(haystack, prefix, prefix_len) == 0;
}

static inline bool xr_string_core_ends_with(const char *haystack, size_t haystack_len,
                                            const char *suffix, size_t suffix_len) {
    if ((!haystack && haystack_len != 0) || (!suffix && suffix_len != 0))
        return false;
    if (suffix_len > haystack_len)
        return false;
    if (suffix_len == 0)
        return true;
    return memcmp(haystack + haystack_len - suffix_len, suffix, suffix_len) == 0;
}

static inline char xr_string_core_ascii_lower_byte(char c) {
    return (c >= 'A' && c <= 'Z') ? (char) (c + ('a' - 'A')) : c;
}

static inline char xr_string_core_ascii_upper_byte(char c) {
    return (c >= 'a' && c <= 'z') ? (char) (c - ('a' - 'A')) : c;
}

static inline size_t xr_string_core_ascii_lower_write(char *out, const char *data, size_t len) {
    if (!out)
        return 0;
    if ((!data && len != 0) || len == 0) {
        out[0] = '\0';
        return 0;
    }
    for (size_t i = 0; i < len; i++)
        out[i] = xr_string_core_ascii_lower_byte(data[i]);
    out[len] = '\0';
    return len;
}

static inline size_t xr_string_core_ascii_upper_write(char *out, const char *data, size_t len) {
    if (!out)
        return 0;
    if ((!data && len != 0) || len == 0) {
        out[0] = '\0';
        return 0;
    }
    for (size_t i = 0; i < len; i++)
        out[i] = xr_string_core_ascii_upper_byte(data[i]);
    out[len] = '\0';
    return len;
}

static inline size_t xr_string_core_reverse_utf8_write(char *out, const char *data, size_t len) {
    if (!out)
        return 0;
    if ((!data && len != 0) || len == 0) {
        out[0] = '\0';
        return 0;
    }

    size_t dst = 0;
    size_t end = len;
    while (end > 0) {
        size_t start = end - 1;
        while (start > 0 && ((unsigned char) data[start] & 0xC0u) == 0x80u)
            start--;
        size_t char_len = end - start;
        memcpy(out + dst, data + start, char_len);
        dst += char_len;
        end = start;
    }
    out[dst] = '\0';
    return dst;
}

#endif  // XRAY_SHARED_XR_STRING_CORE_H
