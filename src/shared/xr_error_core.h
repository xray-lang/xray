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
#define XR_ERROR_CORE_TYPE_MISMATCH_BUFSZ 160
#define XR_ERROR_CORE_DIVISION_BY_ZERO_MSG "division by zero"
#define XR_ERROR_CORE_MODULO_BY_ZERO_MSG "modulo by zero"
#define XR_ERROR_CORE_MODULO_REQUIRES_INTEGER_MSG "modulo requires integer types"
#define XR_ERROR_CORE_BYTES_CONSTRUCTOR_EXPECTS_MSG "Bytes(n): n must be integer or array"
#define XR_ERROR_CORE_BYTES_CONSTRUCTOR_FILL_EXPECTS_MSG                                           \
    "Bytes(n, value): both args must be integers"
#define XR_ERROR_CORE_SLICE_BOUNDS_EXPECTS_MSG "slice bounds must be integers"
#define XR_ERROR_CORE_ARRAY_CAPACITY_EXPECTS_MSG "Array capacity must be an integer"
#define XR_ERROR_CORE_ARRAY_RESERVE_EXPECTS_MSG "Array.reserve(capacity) expects an integer"
#define XR_ERROR_CORE_ARRAY_RESERVE_FAILED_MSG "Array.reserve failed"
#define XR_ERROR_CORE_ARRAY_RESIZE_EXPECTS_MSG "Array.resize(length, fill) expects integer length"
#define XR_ERROR_CORE_ARRAY_RESIZE_FAILED_MSG "Array.resize failed"
#define XR_ERROR_CORE_BYTES_LOAD_U32_EXPECTS_MSG "Bytes.loadU32LE(offset) expects Bytes and integer"
#define XR_ERROR_CORE_BYTES_LOAD_U32_RECEIVER_MSG "Bytes.loadU32LE receiver must be Bytes"
#define XR_ERROR_CORE_BYTES_LOAD_U32_OOB_MSG "Bytes.loadU32LE offset out of bounds"
#define XR_ERROR_CORE_BYTES_LOAD_U64_EXPECTS_MSG "Bytes.loadU64LE(offset) expects Bytes and integer"
#define XR_ERROR_CORE_BYTES_LOAD_U64_RECEIVER_MSG "Bytes.loadU64LE receiver must be Bytes"
#define XR_ERROR_CORE_BYTES_LOAD_U64_OOB_MSG "Bytes.loadU64LE offset out of bounds"
#define XR_ERROR_CORE_BYTES_COPY_WITHIN_EXPECTS_MSG                                                \
    "Bytes.copyWithin expects integer offsets and count"
#define XR_ERROR_CORE_BYTES_COPY_WITHIN_RECEIVER_MSG "Bytes.copyWithin receiver must be Bytes"
#define XR_ERROR_CORE_BYTES_COPY_WITHIN_OOB_MSG "Bytes.copyWithin range out of bounds"
#define XR_ERROR_CORE_BYTES_COPY_FROM_EXPECTS_MSG "Bytes.copyFrom expects Bytes and integer ranges"
#define XR_ERROR_CORE_BYTES_COPY_FROM_OPERANDS_MSG "Bytes.copyFrom operands must be Bytes"
#define XR_ERROR_CORE_BYTES_COPY_FROM_OOB_MSG "Bytes.copyFrom range out of bounds"
#define XR_ERROR_CORE_BYTES_REPEAT_FROM_EXPECTS_MSG                                                \
    "Bytes.repeatFrom expects integer offsets and count"
#define XR_ERROR_CORE_BYTES_REPEAT_FROM_RECEIVER_MSG "Bytes.repeatFrom receiver must be Bytes"
#define XR_ERROR_CORE_BYTES_REPEAT_FROM_OOB_MSG "Bytes.repeatFrom range out of bounds"
#define XR_ERROR_CORE_RANGE_TO_ARRAY_TOO_LARGE_MSG "Range.toArray range too large"

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

static inline int xr_error_core_format_type_mismatch(char *buf, size_t cap, const char *expected,
                                                     const char *actual) {
    return snprintf(buf, cap, "TypeError: expected '%s', got '%s'", expected ? expected : "unknown",
                    actual ? actual : "unknown");
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
