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

#endif  // XRAY_SHARED_XR_STRING_CORE_H
