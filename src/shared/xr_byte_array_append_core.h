/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_byte_array_append_core.h - Canonical growable Array<byte>.appendFrom semantics.
 */

#ifndef XR_BYTE_ARRAY_APPEND_CORE_H
#define XR_BYTE_ARRAY_APPEND_CORE_H

#include "xr_elem_type.h"
#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct XrByteArrayAppendView {
    void *data;
    int64_t length;
    int64_t capacity;
    uint8_t elem_type;
    bool resizable;
    const void *identity;
} XrByteArrayAppendView;

typedef bool (*XrByteArrayAppendReserveFn)(void *ctx, XrByteArrayAppendView *view,
                                           int64_t capacity);

typedef enum XrByteArrayAppendStatus {
    XR_BYTE_ARRAY_APPEND_OK = 0,
    XR_BYTE_ARRAY_APPEND_WRONG_ELEMENT_TYPE = 1,
    XR_BYTE_ARRAY_APPEND_OUT_OF_BOUNDS = 2,
    XR_BYTE_ARRAY_APPEND_NO_DATA = 3,
    XR_BYTE_ARRAY_APPEND_RESERVE_FAILED = 4,
} XrByteArrayAppendStatus;

typedef struct XrByteArrayAppendResult {
    XrByteArrayAppendStatus status;
    int64_t old_length;
    int64_t new_length;
    bool changed;
} XrByteArrayAppendResult;

#define XR_BYTE_ARRAY_APPEND_OWNER_GUARD(owner_hi, owner_lo)                                    \
    ((void) sizeof(struct {                                                                       \
        unsigned int owner_id_must_be_shared_byte_array_append                                  \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_APPEND_HI &&          \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_APPEND_LO)             \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_BYTE_ARRAY_APPEND_CONSUMER_GUARD(consumer_bit)                                       \
    ((void) sizeof(struct {                                                                       \
        unsigned int consumer_must_be_declared_for_shared_byte_array_append                     \
            : (((uint32_t) (consumer_bit) != 0 &&                                                \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&          \
                (XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_APPEND_CONSUMERS &                            \
                 (uint32_t) (consumer_bit)) != 0)                                                \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_BYTE_ARRAY_APPEND_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)           \
    (XR_BYTE_ARRAY_APPEND_OWNER_GUARD((owner_hi), (owner_lo)),                                   \
     XR_BYTE_ARRAY_APPEND_CONSUMER_GUARD((consumer_bit)), (expression))

static inline XrByteArrayAppendResult xr_byte_array_append_core(
    XrByteArrayAppendView *dst, const void *src_data, int64_t src_length, uint8_t src_elem_type,
    const void *src_guard, XrByteArrayAppendReserveFn reserve_fn, void *reserve_ctx) {
    XrByteArrayAppendResult result = {XR_BYTE_ARRAY_APPEND_OUT_OF_BOUNDS, 0, 0, false};
    if (!dst)
        return result;

    result.old_length = dst->length;
    result.new_length = dst->length;
    if (dst->elem_type != XR_ELEM_U8 || src_elem_type != XR_ELEM_U8) {
        result.status = XR_BYTE_ARRAY_APPEND_WRONG_ELEMENT_TYPE;
        return result;
    }
    if (!dst->resizable || dst->length < 0 || dst->capacity < dst->length || src_length < 0 ||
        src_length > INT32_MAX || dst->length > INT32_MAX - src_length)
        return result;
    if (src_length > 0 && !src_data) {
        result.status = XR_BYTE_ARRAY_APPEND_NO_DATA;
        return result;
    }

    bool aliases_dst = false;
    int64_t src_offset = 0;
    if (src_length > 0 && dst->data) {
        uintptr_t base = (uintptr_t) dst->data;
        uintptr_t source = (uintptr_t) src_data;
        uintptr_t end = base + (uintptr_t) dst->length;
        if (end < base)
            return result;
        if (source >= base && source <= end) {
            uintptr_t offset = source - base;
            if (offset > (uintptr_t) dst->length ||
                (uintptr_t) src_length > (uintptr_t) dst->length - offset)
                return result;
            src_offset = (int64_t) offset;
            aliases_dst = true;
        } else if (src_guard && src_guard == dst->identity) {
            return result;
        }
    }

    result.new_length = dst->length + src_length;
    if (result.new_length > dst->capacity) {
        if (!reserve_fn || !reserve_fn(reserve_ctx, dst, result.new_length)) {
            result.status = XR_BYTE_ARRAY_APPEND_RESERVE_FAILED;
            return result;
        }
    }
    if (dst->capacity < result.new_length) {
        result.status = XR_BYTE_ARRAY_APPEND_RESERVE_FAILED;
        return result;
    }
    if (result.new_length > 0 && !dst->data) {
        result.status = XR_BYTE_ARRAY_APPEND_NO_DATA;
        return result;
    }

    if (src_length > 0) {
        const uint8_t *source = aliases_dst ? (const uint8_t *) dst->data + src_offset
                                            : (const uint8_t *) src_data;
        memmove((uint8_t *) dst->data + dst->length, source, (size_t) src_length);
    }
    dst->length = result.new_length;
    result.changed = src_length > 0;
    result.status = XR_BYTE_ARRAY_APPEND_OK;
    return result;
}

#endif /* XR_BYTE_ARRAY_APPEND_CORE_H */
