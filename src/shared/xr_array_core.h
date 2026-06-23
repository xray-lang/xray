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

typedef enum XrArrayCoreIndexSetKind {
    XR_ARRAY_CORE_INDEX_SET_INVALID = 0,
    XR_ARRAY_CORE_INDEX_SET_WRITE,
} XrArrayCoreIndexSetKind;

typedef struct XrArrayCoreIndexSetPlan {
    XrArrayCoreIndexSetKind kind;
    int64_t index;
} XrArrayCoreIndexSetPlan;

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

static inline XrArrayCoreIndexSetPlan xr_array_core_index_set_plan(int64_t length, int64_t index) {
    XrArrayCoreIndexSetPlan out = {XR_ARRAY_CORE_INDEX_SET_INVALID, index};
    if (length < 0)
        length = 0;
    if (index < 0)
        return out;
    if (index < length) {
        out.kind = XR_ARRAY_CORE_INDEX_SET_WRITE;
        return out;
    }
    return out;
}

static inline bool xr_array_core_reverse(void *data, int64_t length, uint8_t elem_size) {
    if (length < 2)
        return true;
    if (!data || elem_size == 0)
        return false;

    uint8_t *bytes = (uint8_t *) data;
    int64_t left = 0;
    int64_t right = length - 1;
    while (left < right) {
        uint8_t *lp = bytes + (size_t) left * elem_size;
        uint8_t *rp = bytes + (size_t) right * elem_size;
        for (uint8_t i = 0; i < elem_size; i++) {
            uint8_t tmp = lp[i];
            lp[i] = rp[i];
            rp[i] = tmp;
        }
        left++;
        right--;
    }
    return true;
}

static inline bool xr_array_core_shift_left_one(void *data, int64_t length, uint8_t elem_size) {
    if (length < 2)
        return true;
    if (!data || elem_size == 0)
        return false;

    memmove(data, (uint8_t *) data + elem_size, (size_t) (length - 1) * elem_size);
    return true;
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

static inline bool xr_array_core_bytes_range_ok(int64_t length, uint8_t elem_type, int64_t offset,
                                                int64_t count) {
    if (length < 0 || elem_type != XR_ELEM_U8 || offset < 0 || count < 0)
        return false;
    return count <= length && offset <= length - count;
}

static inline uint32_t xr_array_core_bytes_load_u32_le(const void *data, int64_t length,
                                                       uint8_t elem_type, int64_t offset,
                                                       bool *ok) {
    bool valid = data && xr_array_core_bytes_range_ok(length, elem_type, offset, 4);
    if (ok)
        *ok = valid;
    if (!valid)
        return 0;
    const uint8_t *p = (const uint8_t *) data + offset;
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}

static inline uint64_t xr_array_core_bytes_load_u64_le(const void *data, int64_t length,
                                                       uint8_t elem_type, int64_t offset,
                                                       bool *ok) {
    bool valid = data && xr_array_core_bytes_range_ok(length, elem_type, offset, 8);
    if (ok)
        *ok = valid;
    if (!valid)
        return 0;
    const uint8_t *p = (const uint8_t *) data + offset;
    return (uint64_t) p[0] | ((uint64_t) p[1] << 8) | ((uint64_t) p[2] << 16) |
           ((uint64_t) p[3] << 24) | ((uint64_t) p[4] << 32) | ((uint64_t) p[5] << 40) |
           ((uint64_t) p[6] << 48) | ((uint64_t) p[7] << 56);
}

static inline bool xr_array_core_bytes_copy_within(void *data, int64_t length, uint8_t elem_type,
                                                   int64_t dst_offset, int64_t src_offset,
                                                   int64_t count) {
    if (!xr_array_core_bytes_range_ok(length, elem_type, src_offset, count) ||
        !xr_array_core_bytes_range_ok(length, elem_type, dst_offset, count))
        return false;
    if (count == 0)
        return true;
    if (!data)
        return false;
    memmove((uint8_t *) data + dst_offset, (uint8_t *) data + src_offset, (size_t) count);
    return true;
}

static inline bool xr_array_core_bytes_copy_from(void *dst_data, int64_t dst_length,
                                                 uint8_t dst_elem_type, const void *src_data,
                                                 int64_t src_length, uint8_t src_elem_type,
                                                 int64_t src_offset, int64_t dst_offset,
                                                 int64_t count, bool same_array) {
    if (!xr_array_core_bytes_range_ok(src_length, src_elem_type, src_offset, count) ||
        !xr_array_core_bytes_range_ok(dst_length, dst_elem_type, dst_offset, count))
        return false;
    if (count == 0)
        return true;
    if (!dst_data || !src_data)
        return false;
    uint8_t *dp = (uint8_t *) dst_data + dst_offset;
    const uint8_t *sp = (const uint8_t *) src_data + src_offset;
    if (same_array)
        memmove(dp, sp, (size_t) count);
    else
        memcpy(dp, sp, (size_t) count);
    return true;
}

static inline bool xr_array_core_bytes_repeat_from(void *data, int64_t length, uint8_t elem_type,
                                                   int64_t dst_offset, int64_t distance,
                                                   int64_t count) {
    if (elem_type != XR_ELEM_U8 || dst_offset < 0 || distance <= 0 || count < 0)
        return false;
    if (distance > dst_offset)
        return false;
    if (!xr_array_core_bytes_range_ok(length, elem_type, dst_offset, count))
        return false;
    if (count == 0)
        return true;
    if (!data)
        return false;
    uint8_t *bytes = (uint8_t *) data;
    for (int64_t i = 0; i < count; i++)
        bytes[dst_offset + i] = bytes[dst_offset - distance + i];
    return true;
}

#endif  // XR_ARRAY_CORE_H
