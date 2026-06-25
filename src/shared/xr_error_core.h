/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_error_core.h - Runtime-neutral user-visible error formatting helpers.
 */

#ifndef XR_ERROR_CORE_H
#define XR_ERROR_CORE_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define XR_ERROR_CORE_INDEX_OOB_BUFSZ 96

typedef struct XrErrorCoreMessageView {
    int code;
    const char *message;
    size_t message_len;
    bool has_code;
} XrErrorCoreMessageView;

static inline int xr_error_core_format_array_index_oob(char *buf, size_t cap, int64_t index,
                                                       int64_t length) {
    return snprintf(buf, cap, "array index out of range: %" PRId64 " (length %" PRId64 ")", index,
                    length);
}

static inline int xr_error_core_format_prefixed(char *buf, size_t cap, int code,
                                                const char *message) {
    return snprintf(buf, cap, "E%04d: %s", code, message ? message : "");
}

static inline XrErrorCoreMessageView xr_error_core_parse_prefixed(const char *data, size_t len) {
    XrErrorCoreMessageView view = {0, data, len, false};
    if (!data || len < 7 || data[0] != 'E' || data[5] != ':' || data[6] != ' ')
        return view;

    int code = 0;
    for (size_t i = 1; i < 5; i++) {
        if (data[i] < '0' || data[i] > '9')
            return view;
        code = code * 10 + (data[i] - '0');
    }

    view.code = code;
    view.message = data + 7;
    view.message_len = len - 7;
    view.has_code = true;
    return view;
}

#endif  // XR_ERROR_CORE_H
