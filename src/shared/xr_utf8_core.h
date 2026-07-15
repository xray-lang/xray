/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_utf8_core.h - Runtime-neutral UTF-8 diagnostic and replacement core
 */

#ifndef XRAY_SHARED_XR_UTF8_CORE_H
#define XRAY_SHARED_XR_UTF8_CORE_H

#include <stddef.h>
#include <stdint.h>

typedef enum XrUtf8ErrorKind {
    XR_UTF8_OK = 0,
    XR_UTF8_OVERLONG,
    XR_UTF8_SURROGATE,
    XR_UTF8_TRUNCATED,
    XR_UTF8_STRAY_CONTINUATION,
    XR_UTF8_OUT_OF_RANGE,
} XrUtf8ErrorKind;

typedef struct XrUtf8ScanResult {
    XrUtf8ErrorKind error;
    size_t byte_offset;
    size_t invalid_length;
    size_t rune_count;
} XrUtf8ScanResult;

typedef struct XrUtf8Step {
    uint32_t scalar;
    size_t consumed;
    XrUtf8ErrorKind error;
} XrUtf8Step;

#define XR_UTF8_CORE_REPLACEMENT UINT32_C(0xFFFD)

static inline int xr_utf8_core_is_continuation(uint8_t byte) {
    return (byte & UINT8_C(0xC0)) == UINT8_C(0x80);
}

/*
 * Decode one scalar or one maximal subpart. On malformed input, consumed is
 * the longest prefix that can still begin a well-formed UTF-8 scalar (and is
 * always at least one for non-empty input). This is the Unicode D93b advance
 * rule used by lossy conversion; strict diagnostics may report a wider
 * semantically invalid sequence through xr_utf8_core_scan_strict().
 */
static inline XrUtf8Step xr_utf8_core_decode_step(const uint8_t *data, size_t len) {
    XrUtf8Step out = {XR_UTF8_CORE_REPLACEMENT, 0, XR_UTF8_TRUNCATED};
    if (!data || len == 0)
        return out;

    uint8_t b0 = data[0];
    if (b0 <= UINT8_C(0x7F)) {
        out.scalar = b0;
        out.consumed = 1;
        out.error = XR_UTF8_OK;
        return out;
    }

    size_t expected = 0;
    uint8_t second_min = UINT8_C(0x80);
    uint8_t second_max = UINT8_C(0xBF);
    if (b0 >= UINT8_C(0xC2) && b0 <= UINT8_C(0xDF)) {
        expected = 2;
    } else if (b0 >= UINT8_C(0xE0) && b0 <= UINT8_C(0xEF)) {
        expected = 3;
        if (b0 == UINT8_C(0xE0))
            second_min = UINT8_C(0xA0);
        else if (b0 == UINT8_C(0xED))
            second_max = UINT8_C(0x9F);
    } else if (b0 >= UINT8_C(0xF0) && b0 <= UINT8_C(0xF4)) {
        expected = 4;
        if (b0 == UINT8_C(0xF0))
            second_min = UINT8_C(0x90);
        else if (b0 == UINT8_C(0xF4))
            second_max = UINT8_C(0x8F);
    } else {
        out.consumed = 1;
        if (xr_utf8_core_is_continuation(b0))
            out.error = XR_UTF8_STRAY_CONTINUATION;
        else if (b0 == UINT8_C(0xC0) || b0 == UINT8_C(0xC1))
            out.error = XR_UTF8_OVERLONG;
        else
            out.error = XR_UTF8_OUT_OF_RANGE;
        return out;
    }

    out.consumed = 1;
    if (len < 2)
        return out;

    uint8_t b1 = data[1];
    if (b1 < second_min || b1 > second_max) {
        if (!xr_utf8_core_is_continuation(b1)) {
            out.error = XR_UTF8_TRUNCATED;
        } else if (b0 == UINT8_C(0xE0) || b0 == UINT8_C(0xF0)) {
            out.error = XR_UTF8_OVERLONG;
        } else if (b0 == UINT8_C(0xED)) {
            out.error = XR_UTF8_SURROGATE;
        } else {
            out.error = XR_UTF8_OUT_OF_RANGE;
        }
        return out;
    }

    if (expected == 2) {
        out.scalar = ((uint32_t) (b0 & UINT8_C(0x1F)) << 6) | (uint32_t) (b1 & UINT8_C(0x3F));
        out.consumed = 2;
        out.error = XR_UTF8_OK;
        return out;
    }

    out.consumed = 2;
    if (len < 3 || !xr_utf8_core_is_continuation(data[2]))
        return out;

    uint8_t b2 = data[2];
    if (expected == 3) {
        out.scalar = ((uint32_t) (b0 & UINT8_C(0x0F)) << 12) |
                     ((uint32_t) (b1 & UINT8_C(0x3F)) << 6) | (uint32_t) (b2 & UINT8_C(0x3F));
        out.consumed = 3;
        out.error = XR_UTF8_OK;
        return out;
    }

    out.consumed = 3;
    if (len < 4 || !xr_utf8_core_is_continuation(data[3]))
        return out;

    uint8_t b3 = data[3];
    out.scalar = ((uint32_t) (b0 & UINT8_C(0x07)) << 18) | ((uint32_t) (b1 & UINT8_C(0x3F)) << 12) |
                 ((uint32_t) (b2 & UINT8_C(0x3F)) << 6) | (uint32_t) (b3 & UINT8_C(0x3F));
    out.consumed = 4;
    out.error = XR_UTF8_OK;
    return out;
}

static inline size_t xr_utf8_core_diagnostic_length(const uint8_t *data, size_t len,
                                                    XrUtf8Step step) {
    if (!data || len == 0)
        return 0;
    if (step.error == XR_UTF8_TRUNCATED || step.error == XR_UTF8_STRAY_CONTINUATION)
        return step.consumed;

    size_t expected = 1;
    uint8_t b0 = data[0];
    if (b0 == UINT8_C(0xC0) || b0 == UINT8_C(0xC1))
        expected = 2;
    else if (b0 >= UINT8_C(0xE0) && b0 <= UINT8_C(0xEF))
        expected = 3;
    else if (b0 >= UINT8_C(0xF0) && b0 <= UINT8_C(0xF7))
        expected = 4;

    size_t actual = 1;
    while (actual < expected && actual < len && xr_utf8_core_is_continuation(data[actual]))
        actual++;
    return actual;
}

static inline XrUtf8ScanResult xr_utf8_core_scan_strict(const uint8_t *data, size_t len) {
    XrUtf8ScanResult out = {XR_UTF8_OK, 0, 0, 0};
    if (!data) {
        if (len != 0)
            out.error = XR_UTF8_TRUNCATED;
        return out;
    }

    size_t pos = 0;
    while (pos < len) {
        XrUtf8Step step = xr_utf8_core_decode_step(data + pos, len - pos);
        if (step.error != XR_UTF8_OK) {
            out.error = step.error;
            out.byte_offset = pos;
            out.invalid_length = xr_utf8_core_diagnostic_length(data + pos, len - pos, step);
            return out;
        }
        pos += step.consumed;
        out.rune_count++;
    }

    out.byte_offset = len;
    return out;
}

#endif /* XRAY_SHARED_XR_UTF8_CORE_H */
