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
#include "xr_typed_ops.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
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

typedef bool (*XrArrayCoreJoinElementFn)(void *ctx, int64_t index, char *dst, size_t *len);
typedef XrValue (*XrArrayCoreReadFn)(void *ctx, int64_t index);
typedef XrValue (*XrArrayCoreMapFn)(void *ctx, XrValue value);
typedef bool (*XrArrayCoreWriteFn)(void *ctx, int64_t index, XrValue value);
typedef bool (*XrArrayCorePredicateFn)(void *ctx, XrValue value);
typedef XrValue (*XrArrayCoreReduceFn)(void *ctx, XrValue acc, XrValue value);
typedef bool (*XrArrayCoreEachFn)(void *ctx, XrValue value);

typedef struct XrArrayCoreFindResult {
    bool found;
    int64_t index;
    XrValue value;
} XrArrayCoreFindResult;

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

static inline bool xr_array_core_join_total(int64_t length, size_t sep_len,
                                            XrArrayCoreJoinElementFn element_fn, void *ctx,
                                            size_t *out_total) {
    if (!out_total || !element_fn)
        return false;
    *out_total = 0;
    if (length <= 0)
        return true;

    size_t total = 0;
    for (int64_t i = 0; i < length; i++) {
        size_t elem_len = 0;
        if (!element_fn(ctx, i, NULL, &elem_len))
            return false;
        if (elem_len > SIZE_MAX - total)
            return false;
        total += elem_len;
        if (i < length - 1) {
            if (sep_len > SIZE_MAX - total)
                return false;
            total += sep_len;
        }
    }

    *out_total = total;
    return true;
}

static inline bool xr_array_core_join_write(char *dst, size_t capacity, int64_t length,
                                            const char *sep, size_t sep_len,
                                            XrArrayCoreJoinElementFn element_fn, void *ctx,
                                            size_t *out_written) {
    if (out_written)
        *out_written = 0;
    if (!dst || capacity == 0 || !element_fn)
        return false;
    if (length <= 0) {
        dst[0] = '\0';
        return true;
    }

    size_t pos = 0;
    for (int64_t i = 0; i < length; i++) {
        size_t elem_len = 0;
        if (!element_fn(ctx, i, NULL, &elem_len))
            return false;
        if (elem_len >= capacity - pos)
            return false;
        size_t written_len = 0;
        if (!element_fn(ctx, i, dst + pos, &written_len))
            return false;
        if (written_len != elem_len)
            return false;
        pos += written_len;

        if (i < length - 1 && sep_len > 0) {
            if (!sep || sep_len >= capacity - pos)
                return false;
            memcpy(dst + pos, sep, sep_len);
            pos += sep_len;
        }
    }

    dst[pos] = '\0';
    if (out_written)
        *out_written = pos;
    return true;
}

static inline bool xr_array_core_hof_map(int64_t length, XrArrayCoreReadFn read_fn,
                                         XrArrayCoreMapFn map_fn, XrArrayCoreWriteFn write_fn,
                                         void *ctx, int64_t *out_count) {
    if (out_count)
        *out_count = 0;
    if (length <= 0)
        return true;
    if (!read_fn || !map_fn || !write_fn)
        return false;

    for (int64_t i = 0; i < length; i++) {
        XrValue mapped = map_fn(ctx, read_fn(ctx, i));
        if (!write_fn(ctx, i, mapped))
            return false;
    }
    if (out_count)
        *out_count = length;
    return true;
}

static inline bool xr_array_core_hof_filter(int64_t length, XrArrayCoreReadFn read_fn,
                                            XrArrayCorePredicateFn predicate_fn,
                                            XrArrayCoreWriteFn write_fn, void *ctx,
                                            int64_t *out_count) {
    if (out_count)
        *out_count = 0;
    if (length <= 0)
        return true;
    if (!read_fn || !predicate_fn || !write_fn)
        return false;

    int64_t kept = 0;
    for (int64_t i = 0; i < length; i++) {
        XrValue value = read_fn(ctx, i);
        if (!predicate_fn(ctx, value))
            continue;
        if (!write_fn(ctx, kept, value))
            return false;
        kept++;
    }
    if (out_count)
        *out_count = kept;
    return true;
}

static inline XrValue xr_array_core_hof_reduce(int64_t length, XrArrayCoreReadFn read_fn,
                                               XrArrayCoreReduceFn reduce_fn, void *ctx,
                                               XrValue initial) {
    XrValue acc = initial;
    if (length <= 0 || !read_fn || !reduce_fn)
        return acc;

    for (int64_t i = 0; i < length; i++)
        acc = reduce_fn(ctx, acc, read_fn(ctx, i));
    return acc;
}

static inline bool xr_array_core_hof_for_each(int64_t length, XrArrayCoreReadFn read_fn,
                                              XrArrayCoreEachFn each_fn, void *ctx,
                                              int64_t *out_count) {
    if (out_count)
        *out_count = 0;
    if (length <= 0)
        return true;
    if (!read_fn || !each_fn)
        return false;

    for (int64_t i = 0; i < length; i++) {
        if (!each_fn(ctx, read_fn(ctx, i)))
            return false;
    }
    if (out_count)
        *out_count = length;
    return true;
}

static inline XrArrayCoreFindResult xr_array_core_hof_find(int64_t length,
                                                           XrArrayCoreReadFn read_fn,
                                                           XrArrayCorePredicateFn predicate_fn,
                                                           void *ctx) {
    XrArrayCoreFindResult result = {false, -1, XR_NULL_VAL};
    if (length <= 0 || !read_fn || !predicate_fn)
        return result;

    for (int64_t i = 0; i < length; i++) {
        XrValue value = read_fn(ctx, i);
        if (predicate_fn(ctx, value)) {
            result.found = true;
            result.index = i;
            result.value = value;
            return result;
        }
    }
    return result;
}

static inline int64_t xr_array_core_hof_find_index(int64_t length, XrArrayCoreReadFn read_fn,
                                                   XrArrayCorePredicateFn predicate_fn, void *ctx) {
    return xr_array_core_hof_find(length, read_fn, predicate_fn, ctx).index;
}

static inline bool xr_array_core_hof_every(int64_t length, XrArrayCoreReadFn read_fn,
                                           XrArrayCorePredicateFn predicate_fn, void *ctx) {
    if (length <= 0)
        return true;
    if (!read_fn || !predicate_fn)
        return false;

    for (int64_t i = 0; i < length; i++) {
        if (!predicate_fn(ctx, read_fn(ctx, i)))
            return false;
    }
    return true;
}

static inline bool xr_array_core_hof_some(int64_t length, XrArrayCoreReadFn read_fn,
                                          XrArrayCorePredicateFn predicate_fn, void *ctx) {
    if (length <= 0 || !read_fn || !predicate_fn)
        return false;

    for (int64_t i = 0; i < length; i++) {
        if (predicate_fn(ctx, read_fn(ctx, i)))
            return true;
    }
    return false;
}

#define XR_ARRAY_CORE_FILL_TYPED_LOOP(T, expr)                                                     \
    do {                                                                                           \
        T *d = (T *) data;                                                                         \
        T fill_value = (T) (expr);                                                                 \
        for (int64_t i = start; i < end; i++)                                                      \
            d[i] = fill_value;                                                                     \
        return true;                                                                               \
    } while (0)

static inline bool xr_array_core_fill_typed_storage(void *data, int64_t start, int64_t count,
                                                    uint8_t elem_type, XrValue value) {
    if (count <= 0)
        return true;
    if (start < 0 || !data || start > INT64_MAX - count)
        return false;

    int64_t end = start + count;
    switch (elem_type) {
        case XR_ELEM_ANY:
            return false;
        case XR_ELEM_I8:
        case XR_ELEM_U8: {
            uint8_t byte = (uint8_t) xr_value_to_int64_coerce(value);
            memset((uint8_t *) data + (size_t) start, byte, (size_t) count);
            return true;
        }
        case XR_ELEM_BOOL: {
            bool falsy = XR_IS_FALSE(value) || XR_IS_NULL(value) ||
                         (XR_IS_INT(value) && XR_TO_INT(value) == 0) ||
                         (XR_IS_FLOAT(value) && XR_TO_FLOAT(value) == 0.0);
            memset((uint8_t *) data + (size_t) start, falsy ? 0 : 1, (size_t) count);
            return true;
        }
        case XR_ELEM_I16:
            XR_ARRAY_CORE_FILL_TYPED_LOOP(int16_t, xr_value_to_int64_coerce(value));
        case XR_ELEM_U16:
            XR_ARRAY_CORE_FILL_TYPED_LOOP(uint16_t, xr_value_to_int64_coerce(value));
        case XR_ELEM_I32:
            XR_ARRAY_CORE_FILL_TYPED_LOOP(int32_t, xr_value_to_int64_coerce(value));
        case XR_ELEM_U32:
            XR_ARRAY_CORE_FILL_TYPED_LOOP(uint32_t, xr_value_to_int64_coerce(value));
        case XR_ELEM_I64:
            XR_ARRAY_CORE_FILL_TYPED_LOOP(int64_t, xr_value_to_int64_coerce(value));
        case XR_ELEM_U64:
            XR_ARRAY_CORE_FILL_TYPED_LOOP(uint64_t, xr_value_to_int64_coerce(value));
        case XR_ELEM_F32:
            XR_ARRAY_CORE_FILL_TYPED_LOOP(float, xr_value_to_f64_coerce(value));
        case XR_ELEM_F64:
            XR_ARRAY_CORE_FILL_TYPED_LOOP(double, xr_value_to_f64_coerce(value));
        default:
            return false;
    }
}

#undef XR_ARRAY_CORE_FILL_TYPED_LOOP

static inline int64_t xr_array_core_nonnegative_length(int64_t length) {
    return length < 0 ? 0 : length;
}

static inline uint8_t xr_array_core_byte_from_value(XrValue value) {
    if (XR_IS_INT(value))
        return (uint8_t) (XR_TO_INT(value) & 0xFF);
    if (XR_IS_FLOAT(value))
        return (uint8_t) ((int64_t) XR_TO_FLOAT(value) & 0xFF);
    if (XR_IS_BOOL(value))
        return XR_IS_FALSE(value) ? 0 : 1;
    return 0;
}

static inline bool xr_array_core_bytes_fill_value(void *dst_data, int64_t length,
                                                  XrValue fill_value) {
    if (length <= 0)
        return true;
    if (!dst_data)
        return false;
    memset(dst_data, xr_array_core_byte_from_value(fill_value), (size_t) length);
    return true;
}

static inline bool xr_array_core_bytes_copy_from_typed(void *dst_data, int64_t dst_length,
                                                       const void *src_data, int64_t src_length,
                                                       uint8_t src_elem_type) {
    if (dst_length <= 0)
        return true;
    if (!dst_data || !src_data || src_length < dst_length || dst_length > INT32_MAX)
        return false;
    if (src_elem_type == XR_ELEM_U8) {
        memcpy(dst_data, src_data, (size_t) dst_length);
        return true;
    }

    uint8_t *dst = (uint8_t *) dst_data;
    void *src_mut = (void *) src_data;
    for (int64_t i = 0; i < dst_length; i++)
        dst[i] = xr_array_core_byte_from_value(xr_typed_get(src_mut, (int32_t) i, src_elem_type));
    return true;
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

static inline bool xr_array_core_shift_right_one(void *data, int64_t length, uint8_t elem_size) {
    if (length <= 0)
        return true;
    if (!data || elem_size == 0)
        return false;

    memmove((uint8_t *) data + elem_size, data, (size_t) length * elem_size);
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
