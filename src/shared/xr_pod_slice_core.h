/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_pod_slice_core.h - Runtime-neutral POD slice and borrowed-view semantics.
 */

#ifndef XR_POD_SLICE_CORE_H
#define XR_POD_SLICE_CORE_H

#if !defined(XR_POD_SLICE_C90)
#include "xr_semantic_owner_ids_gen.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define XR_POD_SLICE_INLINE static inline
#else
#define XR_POD_SLICE_INLINE static
#endif

typedef enum XrPodSliceStatus {
    XR_POD_SLICE_OK = 0,
    XR_POD_SLICE_INVALID_LAYOUT = 1,
    XR_POD_SLICE_BYTE_LENGTH_OVERFLOW = 2,
    XR_POD_SLICE_RANGE_ERROR = 3,
    XR_POD_SLICE_NO_DATA = 4
} XrPodSliceStatus;

typedef enum XrPodSliceFillKind {
    XR_POD_SLICE_FILL_I8 = 0,
    XR_POD_SLICE_FILL_U8 = 1,
    XR_POD_SLICE_FILL_I16 = 2,
    XR_POD_SLICE_FILL_U16 = 3,
    XR_POD_SLICE_FILL_I32 = 4,
    XR_POD_SLICE_FILL_U32 = 5,
    XR_POD_SLICE_FILL_I64 = 6,
    XR_POD_SLICE_FILL_U64 = 7,
    XR_POD_SLICE_FILL_F32 = 8,
    XR_POD_SLICE_FILL_F64 = 9,
    XR_POD_SLICE_FILL_BOOL = 10,
    XR_POD_SLICE_FILL_RUNE = 11,
    XR_POD_SLICE_FILL_RAWPTR = 12
} XrPodSliceFillKind;

typedef union XrPodSliceFillValue {
    uint64_t bits;
    double f64;
    void *ptr;
} XrPodSliceFillValue;

typedef struct XrPodSliceCompareResult {
    int64_t ordering;
    XrPodSliceStatus status;
} XrPodSliceCompareResult;

typedef enum XrPodSliceViewKind {
    XR_POD_SLICE_VIEW_AS_BYTES = 0,
    XR_POD_SLICE_VIEW_REINTERPRET = 1
} XrPodSliceViewKind;

typedef enum XrPodSliceViewStatus {
    XR_POD_SLICE_VIEW_OK = 0,
    XR_POD_SLICE_VIEW_INVALID_SOURCE_LAYOUT = 1,
    XR_POD_SLICE_VIEW_BYTE_LENGTH_OVERFLOW = 2,
    XR_POD_SLICE_VIEW_INVALID_TARGET_LAYOUT = 3,
    XR_POD_SLICE_VIEW_TARGET_SIZE_MISMATCH = 4,
    XR_POD_SLICE_VIEW_LENGTH_NOT_DIVISIBLE = 5,
    XR_POD_SLICE_VIEW_NO_DATA = 6,
    XR_POD_SLICE_VIEW_MISALIGNED = 7
} XrPodSliceViewStatus;

typedef struct XrPodSliceViewResult {
    void *data;
    int64_t length;
    XrPodSliceViewStatus status;
} XrPodSliceViewResult;

#if !defined(XR_POD_SLICE_C90)
#define XR_POD_SLICE_COPY_OWNER_GUARD(owner_hi, owner_lo)                                       \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_pod_slice_copy                                     \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_POD_SLICE_COPY_HI &&            \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_POD_SLICE_COPY_LO)               \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_POD_SLICE_COPY_CONSUMER_GUARD(consumer_bit)                                          \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_pod_slice_copy                        \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (XR_SEM_OWNER_ID_SHARED_POD_SLICE_COPY_CONSUMERS &                               \
                 (uint32_t) (consumer_bit)) != 0)                                                 \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_POD_SLICE_COPY_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)              \
    (XR_POD_SLICE_COPY_OWNER_GUARD((owner_hi), (owner_lo)),                                      \
     XR_POD_SLICE_COPY_CONSUMER_GUARD((consumer_bit)), (expression))

#define XR_POD_SLICE_FILL_OWNER_GUARD(owner_hi, owner_lo)                                       \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_pod_slice_fill                                     \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_POD_SLICE_FILL_HI &&            \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_POD_SLICE_FILL_LO)               \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_POD_SLICE_FILL_CONSUMER_GUARD(consumer_bit)                                          \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_pod_slice_fill                        \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (XR_SEM_OWNER_ID_SHARED_POD_SLICE_FILL_CONSUMERS &                               \
                 (uint32_t) (consumer_bit)) != 0)                                                 \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_POD_SLICE_FILL_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)              \
    (XR_POD_SLICE_FILL_OWNER_GUARD((owner_hi), (owner_lo)),                                      \
     XR_POD_SLICE_FILL_CONSUMER_GUARD((consumer_bit)), (expression))

#define XR_POD_SLICE_COMPARE_OWNER_GUARD(owner_hi, owner_lo)                                    \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_pod_slice_compare                                  \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_POD_SLICE_COMPARE_HI &&         \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_POD_SLICE_COMPARE_LO)            \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_POD_SLICE_COMPARE_CONSUMER_GUARD(consumer_bit)                                       \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_pod_slice_compare                     \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (XR_SEM_OWNER_ID_SHARED_POD_SLICE_COMPARE_CONSUMERS &                            \
                 (uint32_t) (consumer_bit)) != 0)                                                 \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_POD_SLICE_COMPARE_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)           \
    (XR_POD_SLICE_COMPARE_OWNER_GUARD((owner_hi), (owner_lo)),                                   \
     XR_POD_SLICE_COMPARE_CONSUMER_GUARD((consumer_bit)), (expression))

#define XR_POD_SLICE_VIEW_OWNER_GUARD(owner_hi, owner_lo)                                       \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_pod_slice_view                                     \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_HI &&            \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_LO)               \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_POD_SLICE_VIEW_CONSUMER_GUARD(consumer_bit)                                          \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_pod_slice_view                        \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_CONSUMERS &                               \
                 (uint32_t) (consumer_bit)) != 0)                                                 \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_POD_SLICE_VIEW_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)              \
    (XR_POD_SLICE_VIEW_OWNER_GUARD((owner_hi), (owner_lo)),                                      \
     XR_POD_SLICE_VIEW_CONSUMER_GUARD((consumer_bit)), (expression))
#endif

XR_POD_SLICE_INLINE XrPodSliceViewResult xr_pod_slice_view_core(
    XrPodSliceViewKind kind, void *data, int64_t length, uint16_t source_elem_size,
    bool source_has_static_layout, uint16_t target_elem_size, uint16_t target_expected_elem_size,
    uint16_t target_alignment, bool target_layout_valid, bool target_is_aggregate) {
    XrPodSliceViewResult result;
    result.data = data;
    result.length = 0;
    result.status = XR_POD_SLICE_VIEW_OK;
    if (kind == XR_POD_SLICE_VIEW_AS_BYTES) {
        if (!source_has_static_layout || source_elem_size == 0) {
            result.status = XR_POD_SLICE_VIEW_INVALID_SOURCE_LAYOUT;
            return result;
        }
        if (length < 0 || length > INT64_MAX / (int64_t) source_elem_size) {
            result.status = XR_POD_SLICE_VIEW_BYTE_LENGTH_OVERFLOW;
            return result;
        }
        result.length = length * (int64_t) source_elem_size;
        return result;
    }
    if (kind != XR_POD_SLICE_VIEW_REINTERPRET || source_elem_size != 1) {
        result.status = XR_POD_SLICE_VIEW_INVALID_SOURCE_LAYOUT;
        return result;
    }
    if (!target_layout_valid || target_elem_size == 0 || target_alignment == 0) {
        result.status = XR_POD_SLICE_VIEW_INVALID_TARGET_LAYOUT;
        return result;
    }
    if (!target_is_aggregate && target_expected_elem_size != target_elem_size) {
        result.status = XR_POD_SLICE_VIEW_TARGET_SIZE_MISMATCH;
        return result;
    }
    if (length < 0) {
        result.status = XR_POD_SLICE_VIEW_BYTE_LENGTH_OVERFLOW;
        return result;
    }
    if (length % (int64_t) target_elem_size != 0) {
        result.status = XR_POD_SLICE_VIEW_LENGTH_NOT_DIVISIBLE;
        return result;
    }
    if (length > 0 && !data) {
        result.status = XR_POD_SLICE_VIEW_NO_DATA;
        return result;
    }
    if (length > 0 && ((uintptr_t) data % (uintptr_t) target_alignment) != 0) {
        result.status = XR_POD_SLICE_VIEW_MISALIGNED;
        return result;
    }
    result.length = length / (int64_t) target_elem_size;
    return result;
}

XR_POD_SLICE_INLINE XrPodSliceStatus xr_pod_slice_copy_core(
    void *dst_data, int64_t dst_length, uint16_t dst_elem_size, const void *src_data,
    int64_t src_length, uint16_t src_elem_size) {
    int64_t dst_bytes;
    int64_t src_bytes;
    if (dst_elem_size == 0 || src_elem_size == 0 || dst_elem_size != src_elem_size)
        return XR_POD_SLICE_INVALID_LAYOUT;
    if (dst_length < 0 || src_length < 0 ||
        dst_length > INT64_MAX / (int64_t) dst_elem_size ||
        src_length > INT64_MAX / (int64_t) src_elem_size)
        return XR_POD_SLICE_BYTE_LENGTH_OVERFLOW;
    dst_bytes = dst_length * (int64_t) dst_elem_size;
    src_bytes = src_length * (int64_t) src_elem_size;
    if (src_bytes > dst_bytes || (src_bytes > 0 && (!dst_data || !src_data)))
        return XR_POD_SLICE_RANGE_ERROR;
    if (src_bytes > 0)
        memmove(dst_data, src_data, (size_t) src_bytes);
    return XR_POD_SLICE_OK;
}

XR_POD_SLICE_INLINE XrPodSliceStatus xr_pod_slice_fill_core(
    void *data, int64_t length, uint16_t elem_size, XrPodSliceFillKind kind,
    XrPodSliceFillValue value) {
    uint16_t expected_size;
    int64_t i;
    switch (kind) {
        case XR_POD_SLICE_FILL_I8:
        case XR_POD_SLICE_FILL_U8:
        case XR_POD_SLICE_FILL_BOOL:
            expected_size = 1;
            break;
        case XR_POD_SLICE_FILL_I16:
        case XR_POD_SLICE_FILL_U16:
            expected_size = 2;
            break;
        case XR_POD_SLICE_FILL_I32:
        case XR_POD_SLICE_FILL_U32:
        case XR_POD_SLICE_FILL_F32:
        case XR_POD_SLICE_FILL_RUNE:
            expected_size = 4;
            break;
        case XR_POD_SLICE_FILL_I64:
        case XR_POD_SLICE_FILL_U64:
        case XR_POD_SLICE_FILL_F64:
        case XR_POD_SLICE_FILL_RAWPTR:
            expected_size = 8;
            break;
        default:
            return XR_POD_SLICE_INVALID_LAYOUT;
    }
    if (elem_size != expected_size)
        return XR_POD_SLICE_INVALID_LAYOUT;
    if (length < 0 || length > INT64_MAX / (int64_t) elem_size)
        return XR_POD_SLICE_BYTE_LENGTH_OVERFLOW;
    if (length > 0 && !data)
        return XR_POD_SLICE_NO_DATA;
    switch (kind) {
        case XR_POD_SLICE_FILL_I8:
            memset(data, (int8_t) value.bits, (size_t) length);
            break;
        case XR_POD_SLICE_FILL_U8:
            memset(data, (uint8_t) value.bits, (size_t) length);
            break;
        case XR_POD_SLICE_FILL_BOOL:
            memset(data, value.bits ? 1 : 0, (size_t) length);
            break;
        case XR_POD_SLICE_FILL_I16:
            for (i = 0; i < length; ++i)
                ((int16_t *) data)[i] = (int16_t) value.bits;
            break;
        case XR_POD_SLICE_FILL_U16:
            for (i = 0; i < length; ++i)
                ((uint16_t *) data)[i] = (uint16_t) value.bits;
            break;
        case XR_POD_SLICE_FILL_I32:
            for (i = 0; i < length; ++i)
                ((int32_t *) data)[i] = (int32_t) value.bits;
            break;
        case XR_POD_SLICE_FILL_U32:
        case XR_POD_SLICE_FILL_RUNE:
            for (i = 0; i < length; ++i)
                ((uint32_t *) data)[i] = (uint32_t) value.bits;
            break;
        case XR_POD_SLICE_FILL_I64:
            for (i = 0; i < length; ++i)
                ((int64_t *) data)[i] = (int64_t) value.bits;
            break;
        case XR_POD_SLICE_FILL_U64:
            for (i = 0; i < length; ++i)
                ((uint64_t *) data)[i] = value.bits;
            break;
        case XR_POD_SLICE_FILL_F32:
            for (i = 0; i < length; ++i)
                ((float *) data)[i] = (float) value.f64;
            break;
        case XR_POD_SLICE_FILL_F64:
            for (i = 0; i < length; ++i)
                ((double *) data)[i] = value.f64;
            break;
        case XR_POD_SLICE_FILL_RAWPTR:
            for (i = 0; i < length; ++i)
                ((void **) data)[i] = value.ptr;
            break;
        default:
            return XR_POD_SLICE_INVALID_LAYOUT;
    }
    return XR_POD_SLICE_OK;
}

XR_POD_SLICE_INLINE XrPodSliceCompareResult xr_pod_slice_compare_core(
    const void *left_data, int64_t left_length, uint16_t left_elem_size, const void *right_data,
    int64_t right_length, uint16_t right_elem_size) {
    XrPodSliceCompareResult result = {0, XR_POD_SLICE_OK};
    int64_t left_bytes;
    int64_t right_bytes;
    int64_t common_bytes;
    int compared = 0;
    if (left_elem_size == 0 || right_elem_size == 0 || left_elem_size != right_elem_size) {
        result.status = XR_POD_SLICE_INVALID_LAYOUT;
        return result;
    }
    if (left_length < 0 || right_length < 0 ||
        left_length > INT64_MAX / (int64_t) left_elem_size ||
        right_length > INT64_MAX / (int64_t) right_elem_size) {
        result.status = XR_POD_SLICE_BYTE_LENGTH_OVERFLOW;
        return result;
    }
    left_bytes = left_length * (int64_t) left_elem_size;
    right_bytes = right_length * (int64_t) right_elem_size;
    common_bytes = left_bytes < right_bytes ? left_bytes : right_bytes;
    if (common_bytes > 0 && (!left_data || !right_data)) {
        result.status = XR_POD_SLICE_NO_DATA;
        return result;
    }
    if (common_bytes > 0)
        compared = memcmp(left_data, right_data, (size_t) common_bytes);
    if (compared != 0)
        result.ordering = compared < 0 ? -1 : 1;
    else if (left_length != right_length)
        result.ordering = left_length < right_length ? -1 : 1;
    return result;
}

#undef XR_POD_SLICE_INLINE

#endif /* XR_POD_SLICE_CORE_H */
