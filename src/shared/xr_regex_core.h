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
#include <stddef.h>

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
