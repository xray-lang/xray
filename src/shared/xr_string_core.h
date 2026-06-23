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
#include <stddef.h>
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

static inline bool xr_string_core_is_ascii_whitespace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
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
