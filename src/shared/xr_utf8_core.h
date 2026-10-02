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

typedef struct XrUtf8LossyPlan {
    size_t output_length;
    size_t rune_count;
    int overflow;
} XrUtf8LossyPlan;

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
typedef int (*XrUtf8ReadByte)(void *context, const uint8_t *address, uint8_t *output);

static inline int xr_utf8_core_direct_read(void *context, const uint8_t *address, uint8_t *output) {
    (void) context;
    *output = *address;
    return 1;
}

static inline int xr_utf8_core_decode_step_read(const uint8_t *data, size_t len,
                                                XrUtf8ReadByte read, void *context,
                                                XrUtf8Step *result) {
    if (!read || !result) return 0;
    XrUtf8Step out = {XR_UTF8_CORE_REPLACEMENT, 0, XR_UTF8_TRUNCATED};
    if (!data || len == 0)
        do { *result = out; return 1; } while (0);

    uint8_t b0;
    if (!read(context, data + 0, &b0)) return 0;
    if (b0 <= UINT8_C(0x7F)) {
        out.scalar = b0;
        out.consumed = 1;
        out.error = XR_UTF8_OK;
        do { *result = out; return 1; } while (0);
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
        do { *result = out; return 1; } while (0);
    }

    out.consumed = 1;
    if (len < 2)
        do { *result = out; return 1; } while (0);

    uint8_t b1;
    if (!read(context, data + 1, &b1)) return 0;
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
        do { *result = out; return 1; } while (0);
    }

    if (expected == 2) {
        out.scalar = ((uint32_t) (b0 & UINT8_C(0x1F)) << 6) | (uint32_t) (b1 & UINT8_C(0x3F));
        out.consumed = 2;
        out.error = XR_UTF8_OK;
        do { *result = out; return 1; } while (0);
    }

    out.consumed = 2;
    if (len < 3) { *result = out; return 1; }
    uint8_t b2;
    if (!read(context, data + 2, &b2)) return 0;
    if (!xr_utf8_core_is_continuation(b2)) { *result = out; return 1; }
    if (expected == 3) {
        out.scalar = ((uint32_t) (b0 & UINT8_C(0x0F)) << 12) |
                     ((uint32_t) (b1 & UINT8_C(0x3F)) << 6) | (uint32_t) (b2 & UINT8_C(0x3F));
        out.consumed = 3;
        out.error = XR_UTF8_OK;
        do { *result = out; return 1; } while (0);
    }

    out.consumed = 3;
    if (len < 4) { *result = out; return 1; }
    uint8_t b3;
    if (!read(context, data + 3, &b3)) return 0;
    if (!xr_utf8_core_is_continuation(b3)) { *result = out; return 1; }
    out.scalar = ((uint32_t) (b0 & UINT8_C(0x07)) << 18) | ((uint32_t) (b1 & UINT8_C(0x3F)) << 12) |
                 ((uint32_t) (b2 & UINT8_C(0x3F)) << 6) | (uint32_t) (b3 & UINT8_C(0x3F));
    out.consumed = 4;
    out.error = XR_UTF8_OK;
    do { *result = out; return 1; } while (0);
}

static inline int xr_utf8_core_diagnostic_length_read(const uint8_t *data, size_t len,
                                                       XrUtf8Step step, XrUtf8ReadByte read,
                                                       void *context, size_t *output) {
    if (!read || !output) return 0;
    if (!data || !len) { *output = 0; return 1; }
    if (step.error == XR_UTF8_TRUNCATED || step.error == XR_UTF8_STRAY_CONTINUATION) {
        *output = step.consumed;
        return 1;
    }
    uint8_t b0;
    if (!read(context, data, &b0)) return 0;
    size_t expected = 1;
    if (b0 == UINT8_C(0xC0) || b0 == UINT8_C(0xC1)) expected = 2;
    else if (b0 >= UINT8_C(0xE0) && b0 <= UINT8_C(0xEF)) expected = 3;
    else if (b0 >= UINT8_C(0xF0) && b0 <= UINT8_C(0xF7)) expected = 4;
    size_t actual = 1;
    while (actual < expected && actual < len) {
        uint8_t byte;
        if (!read(context, data + actual, &byte)) return 0;
        if (!xr_utf8_core_is_continuation(byte)) break;
        ++actual;
    }
    *output = actual;
    return 1;
}

/* A failed read preserves output and is distinct from malformed UTF-8. */
static inline int xr_utf8_core_scan_strict_read(const uint8_t *data, size_t len,
                                               XrUtf8ReadByte read, void *context,
                                               XrUtf8ScanResult *output) {
    if (!read || !output) return 0;
    XrUtf8ScanResult out = {XR_UTF8_OK, 0, 0, 0};
    if (!data) {
        if (len) out.error = XR_UTF8_TRUNCATED;
        *output = out;
        return 1;
    }
    size_t pos = 0;
    while (pos < len) {
        XrUtf8Step step;
        if (!xr_utf8_core_decode_step_read(data + pos, len - pos, read, context, &step)) return 0;
        if (step.error != XR_UTF8_OK) {
            out.error = step.error;
            out.byte_offset = pos;
            if (!xr_utf8_core_diagnostic_length_read(data + pos, len - pos, step,
                                                       read, context, &out.invalid_length)) return 0;
            *output = out;
            return 1;
        }
        pos += step.consumed;
        ++out.rune_count;
    }
    out.byte_offset = len;
    *output = out;
    return 1;
}

static inline XrUtf8Step xr_utf8_core_decode_step(const uint8_t *data, size_t len) {
    XrUtf8Step result;
    (void) xr_utf8_core_decode_step_read(data, len, xr_utf8_core_direct_read, NULL, &result);
    return result;
}

static inline size_t xr_utf8_core_diagnostic_length(const uint8_t *data, size_t len, XrUtf8Step step) {
    size_t result;
    (void) xr_utf8_core_diagnostic_length_read(data, len, step, xr_utf8_core_direct_read, NULL, &result);
    return result;
}

static inline XrUtf8ScanResult xr_utf8_core_scan_strict(const uint8_t *data, size_t len) {
    XrUtf8ScanResult result;
    (void) xr_utf8_core_scan_strict_read(data, len, xr_utf8_core_direct_read, NULL, &result);
    return result;
}

static inline XrUtf8LossyPlan xr_utf8_core_lossy_plan(const uint8_t *data, size_t len) {
    XrUtf8LossyPlan out = {0, 0, 0};
    if (!data && len != 0) {
        out.overflow = 1;
        return out;
    }

    size_t pos = 0;
    while (pos < len) {
        XrUtf8Step step = xr_utf8_core_decode_step(data + pos, len - pos);
        if (step.consumed == 0) {
            out.overflow = 1;
            return out;
        }
        size_t add = step.error == XR_UTF8_OK ? step.consumed : 3;
        if (out.output_length > (size_t) -1 - add) {
            out.overflow = 1;
            return out;
        }
        out.output_length += add;
        out.rune_count++;
        pos += step.consumed;
    }
    return out;
}

static inline size_t xr_utf8_core_lossy_write(char *out, const uint8_t *data, size_t len) {
    if (!out || (!data && len != 0))
        return 0;

    size_t src = 0;
    size_t dst = 0;
    while (src < len) {
        XrUtf8Step step = xr_utf8_core_decode_step(data + src, len - src);
        if (step.consumed == 0)
            break;
        if (step.error != XR_UTF8_OK) {
            ((unsigned char *) out)[dst++] = 0xEF;
            ((unsigned char *) out)[dst++] = 0xBF;
            ((unsigned char *) out)[dst++] = 0xBD;
        } else {
            for (size_t i = 0; i < step.consumed; i++)
                out[dst++] = (char) data[src + i];
        }
        src += step.consumed;
    }
    return dst;
}

#endif /* XRAY_SHARED_XR_UTF8_CORE_H */
