/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_array_core.h - Runtime-neutral Array method planning helpers.
 */

#ifndef XR_ARRAY_CORE_H
#define XR_ARRAY_CORE_H

#include "xr_elem_type.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct XrArrayCoreRange {
    int64_t start;
    int64_t end;
    int64_t count;
} XrArrayCoreRange;

typedef enum XrArrayCoreNeedleKind {
    XR_ARRAY_CORE_NEEDLE_OTHER = 0,
    XR_ARRAY_CORE_NEEDLE_INT,
    XR_ARRAY_CORE_NEEDLE_FLOAT,
    XR_ARRAY_CORE_NEEDLE_BOOL,
} XrArrayCoreNeedleKind;

typedef struct XrArrayCoreNeedle {
    XrArrayCoreNeedleKind kind;
    int64_t i64;
    double f64;
    uint8_t boolean;
} XrArrayCoreNeedle;

static inline XrArrayCoreNeedle xr_array_core_needle_other(void) {
    return (XrArrayCoreNeedle) {XR_ARRAY_CORE_NEEDLE_OTHER, 0, 0.0, 0};
}

static inline XrArrayCoreNeedle xr_array_core_needle_int(int64_t value) {
    return (XrArrayCoreNeedle) {XR_ARRAY_CORE_NEEDLE_INT, value, 0.0, 0};
}

static inline XrArrayCoreNeedle xr_array_core_needle_float(double value) {
    return (XrArrayCoreNeedle) {XR_ARRAY_CORE_NEEDLE_FLOAT, 0, value, 0};
}

static inline XrArrayCoreNeedle xr_array_core_needle_bool(bool value) {
    return (XrArrayCoreNeedle) {XR_ARRAY_CORE_NEEDLE_BOOL, 0, 0.0, value ? 1 : 0};
}

static inline XrArrayCoreRange xr_array_core_slice_range(int64_t length, int64_t start,
                                                         int64_t end) {
    if (length < 0)
        length = 0;

    if (start < 0)
        start += length;
    if (end < 0)
        end += length;

    if (start < 0)
        start = 0;
    if (start > length)
        start = length;

    if (end < 0)
        end = 0;
    if (end > length)
        end = length;

    if (start > end)
        start = end;

    return (XrArrayCoreRange) {start, end, end - start};
}

static inline XrArrayCoreRange xr_array_core_fill_range(int64_t length, int64_t start,
                                                        int64_t end) {
    return xr_array_core_slice_range(length, start, end);
}

#define XR_ARRAY_CORE_INDEXOF_LOOP(T)                                                              \
    do {                                                                                           \
        const T *d = (const T *) data;                                                             \
        for (int64_t i = 0; i < length; i++)                                                       \
            if ((int64_t) d[i] == needle.i64)                                                      \
                return i;                                                                          \
        return -1;                                                                                 \
    } while (0)

static inline int64_t xr_array_core_typed_index_of(const void *data, int64_t length,
                                                   uint8_t elem_type, XrArrayCoreNeedle needle,
                                                   int *handled) {
    if (handled)
        *handled = 1;

    switch (elem_type) {
        case XR_ELEM_ANY:
            if (handled)
                *handled = 0;
            return -1;
        case XR_ELEM_I8:
        case XR_ELEM_U8:
        case XR_ELEM_I16:
        case XR_ELEM_U16:
        case XR_ELEM_I32:
        case XR_ELEM_U32:
        case XR_ELEM_I64:
        case XR_ELEM_U64:
            if (needle.kind != XR_ARRAY_CORE_NEEDLE_INT)
                return -1;
            break;
        case XR_ELEM_F32:
        case XR_ELEM_F64:
            if (needle.kind != XR_ARRAY_CORE_NEEDLE_FLOAT)
                return -1;
            break;
        case XR_ELEM_BOOL:
            if (needle.kind != XR_ARRAY_CORE_NEEDLE_BOOL)
                return -1;
            break;
        default:
            if (handled)
                *handled = 0;
            return -1;
    }

    if (!data || length <= 0)
        return -1;

    switch (elem_type) {
        case XR_ELEM_I8:
            XR_ARRAY_CORE_INDEXOF_LOOP(int8_t);
        case XR_ELEM_U8: {
            if (needle.i64 < 0 || needle.i64 > 255)
                return -1;
            const void *p = memchr(data, (int) needle.i64, (size_t) length);
            return p ? (int64_t) ((const uint8_t *) p - (const uint8_t *) data) : -1;
        }
        case XR_ELEM_I16:
            XR_ARRAY_CORE_INDEXOF_LOOP(int16_t);
        case XR_ELEM_U16:
            XR_ARRAY_CORE_INDEXOF_LOOP(uint16_t);
        case XR_ELEM_I32:
            XR_ARRAY_CORE_INDEXOF_LOOP(int32_t);
        case XR_ELEM_U32:
            XR_ARRAY_CORE_INDEXOF_LOOP(uint32_t);
        case XR_ELEM_I64:
            XR_ARRAY_CORE_INDEXOF_LOOP(int64_t);
        case XR_ELEM_U64: {
            const uint64_t *d = (const uint64_t *) data;
            for (int64_t i = 0; i < length; i++)
                if ((int64_t) d[i] == needle.i64)
                    return i;
            return -1;
        }
        case XR_ELEM_F32: {
            const float *d = (const float *) data;
            for (int64_t i = 0; i < length; i++)
                if ((double) d[i] == needle.f64)
                    return i;
            return -1;
        }
        case XR_ELEM_F64: {
            const double *d = (const double *) data;
            for (int64_t i = 0; i < length; i++)
                if (d[i] == needle.f64)
                    return i;
            return -1;
        }
        case XR_ELEM_BOOL: {
            const void *p = memchr(data, needle.boolean ? 1 : 0, (size_t) length);
            return p ? (int64_t) ((const uint8_t *) p - (const uint8_t *) data) : -1;
        }
        default:
            if (handled)
                *handled = 0;
            return -1;
    }
}

#undef XR_ARRAY_CORE_INDEXOF_LOOP

#endif  // XR_ARRAY_CORE_H
