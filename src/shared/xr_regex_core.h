/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_regex_core.h - Shared freestanding regex utilities
 */

#ifndef XR_REGEX_CORE_H
#define XR_REGEX_CORE_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    XR_REGEX_CORE_FLAG_NONE = 0,
    XR_REGEX_CORE_FLAG_IGNORECASE = 1 << 0,
    XR_REGEX_CORE_FLAG_MULTILINE = 1 << 1,
    XR_REGEX_CORE_FLAG_DOTALL = 1 << 2,
} XrRegexCoreFlags;

typedef enum {
    XR_REGEX_CORE_MATCH_START = 0,
    XR_REGEX_CORE_MATCH_END = 1,
    XR_REGEX_CORE_MATCH_TEXT = 2,
    XR_REGEX_CORE_MATCH_GROUPS = 3,
    XR_REGEX_CORE_MATCH_FIELD_COUNT = 4,
} XrRegexCoreMatchField;

static const char *const XR_REGEX_CORE_MATCH_FIELD_NAMES[XR_REGEX_CORE_MATCH_FIELD_COUNT] = {
    "start",
    "end",
    "text",
    "groups",
};

enum {
    XR_REGEX_CORE_SPLIT_MAX_PARTS = 256
};

static inline int xr_regex_core_parse_flags(const char *flags, size_t len) {
    if (!flags && len != 0)
        return XR_REGEX_CORE_FLAG_NONE;

    int parsed = XR_REGEX_CORE_FLAG_NONE;
    for (size_t i = 0; i < len; i++) {
        switch (flags[i]) {
            case 'i':
                parsed |= XR_REGEX_CORE_FLAG_IGNORECASE;
                break;
            case 'm':
                parsed |= XR_REGEX_CORE_FLAG_MULTILINE;
                break;
            case 's':
                parsed |= XR_REGEX_CORE_FLAG_DOTALL;
                break;
            default:
                break;
        }
    }
    return parsed;
}

static inline bool xr_regex_core_int_arg(int64_t value, int *out) {
    if (!out || value < (int64_t) INT_MIN || value > (int64_t) INT_MAX)
        return false;
    *out = (int) value;
    return true;
}

static inline int xr_regex_core_limit_arg(bool is_int, int64_t value) {
    if (!is_int)
        return -1;
    if (value > (int64_t) INT_MAX)
        return INT_MAX;
    if (value < (int64_t) INT_MIN)
        return INT_MIN;
    return (int) value;
}

static inline int xr_regex_core_split_max_parts(int limit) {
    return (limit > 0 && limit < XR_REGEX_CORE_SPLIT_MAX_PARTS) ? limit
                                                                : XR_REGEX_CORE_SPLIT_MAX_PARTS;
}

static inline int xr_regex_core_is_special(char c) {
    switch (c) {
        case '\\':
        case '.':
        case '+':
        case '*':
        case '?':
        case '[':
        case ']':
        case '(':
        case ')':
        case '{':
        case '}':
        case '|':
        case '^':
        case '$':
            return 1;
        default:
            return 0;
    }
}

static inline size_t xr_regex_core_escape_len(const char *text, size_t len) {
    if (!text && len != 0)
        return (size_t) -1;
    size_t out_len = 0;
    for (size_t i = 0; i < len; i++) {
        size_t add = xr_regex_core_is_special(text[i]) ? 2u : 1u;
        if (out_len > (size_t) -1 - add)
            return (size_t) -1;
        out_len += add;
    }
    return out_len;
}

static inline int xr_regex_core_escape(const char *text, size_t len, char *out, size_t out_size) {
    if ((!text && len != 0) || !out || out_size == 0)
        return -1;
    size_t needed = xr_regex_core_escape_len(text, len);
    if (needed == (size_t) -1 || needed > (size_t) INT_MAX || out_size <= needed)
        return -1;

    char *o = out;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (xr_regex_core_is_special(c))
            *o++ = '\\';
        *o++ = c;
    }
    *o = '\0';
    return (int) needed;
}

#endif  // XR_REGEX_CORE_H
