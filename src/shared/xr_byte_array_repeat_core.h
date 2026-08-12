/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_byte_array_repeat_core.h - Canonical growable Array<byte>.repeatFrom semantics.
 */

#ifndef XR_BYTE_ARRAY_REPEAT_CORE_H
#define XR_BYTE_ARRAY_REPEAT_CORE_H

#include "xr_byte_slice_scalar_core.h"
#include "xr_elem_type.h"
#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct XrByteArrayRepeatView {
    void *data;
    int64_t length;
    int64_t capacity;
    uint8_t elem_type;
    bool resizable;
} XrByteArrayRepeatView;

typedef bool (*XrByteArrayRepeatReserveFn)(void *ctx, XrByteArrayRepeatView *view,
                                           int64_t capacity);

typedef enum XrByteArrayRepeatStatus {
    XR_BYTE_ARRAY_REPEAT_OK = 0,
    XR_BYTE_ARRAY_REPEAT_WRONG_ELEMENT_TYPE = 1,
    XR_BYTE_ARRAY_REPEAT_OUT_OF_BOUNDS = 2,
    XR_BYTE_ARRAY_REPEAT_NO_DATA = 3,
    XR_BYTE_ARRAY_REPEAT_RESERVE_FAILED = 4,
} XrByteArrayRepeatStatus;

typedef struct XrByteArrayRepeatResult {
    XrByteArrayRepeatStatus status;
    int64_t old_length;
    int64_t new_length;
    bool changed;
} XrByteArrayRepeatResult;

#define XR_BYTE_ARRAY_REPEAT_OWNER_GUARD(owner_hi, owner_lo)                                    \
    ((void) sizeof(struct {                                                                       \
        unsigned int owner_id_must_be_shared_byte_array_repeat                                  \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_REPEAT_HI &&          \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_REPEAT_LO)             \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_BYTE_ARRAY_REPEAT_CONSUMER_GUARD(consumer_bit)                                       \
    ((void) sizeof(struct {                                                                       \
        unsigned int consumer_must_be_declared_for_shared_byte_array_repeat                     \
            : (((uint32_t) (consumer_bit) != 0 &&                                                \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&          \
                (XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_REPEAT_CONSUMERS &                            \
                 (uint32_t) (consumer_bit)) != 0)                                                \
                   ? 1                                                                           \
                   : -1);                                                                        \
    }))

#define XR_BYTE_ARRAY_REPEAT_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)           \
    (XR_BYTE_ARRAY_REPEAT_OWNER_GUARD((owner_hi), (owner_lo)),                                   \
     XR_BYTE_ARRAY_REPEAT_CONSUMER_GUARD((consumer_bit)), (expression))

static inline XrByteArrayRepeatResult xr_byte_array_repeat_tail_core(
    XrByteArrayRepeatView *view, int64_t distance, int64_t count,
    XrByteArrayRepeatReserveFn reserve_fn, void *reserve_ctx) {
    XrByteArrayRepeatResult result = {XR_BYTE_ARRAY_REPEAT_OUT_OF_BOUNDS, 0, 0, false};
    if (!view)
        return result;

    result.old_length = view->length;
    result.new_length = view->length;
    if (view->elem_type != XR_ELEM_U8) {
        result.status = XR_BYTE_ARRAY_REPEAT_WRONG_ELEMENT_TYPE;
        return result;
    }
    if (!view->resizable || view->length < 0 || view->capacity < view->length || distance <= 0 ||
        count < 0 || distance > view->length || count > INT32_MAX ||
        view->length > INT32_MAX - count)
        return result;

    result.new_length = view->length + count;
    if (result.new_length > view->capacity) {
        if (!reserve_fn || !reserve_fn(reserve_ctx, view, result.new_length)) {
            result.status = XR_BYTE_ARRAY_REPEAT_RESERVE_FAILED;
            return result;
        }
    }
    if (view->capacity < result.new_length) {
        result.status = XR_BYTE_ARRAY_REPEAT_RESERVE_FAILED;
        return result;
    }
    if (result.new_length > 0 && !view->data) {
        result.status = XR_BYTE_ARRAY_REPEAT_NO_DATA;
        return result;
    }

    if (count > 0)
        xr_byte_slice_repeat_unchecked(view->data, view->length, distance, count);
    view->length = result.new_length;
    result.changed = count > 0;
    result.status = XR_BYTE_ARRAY_REPEAT_OK;
    return result;
}

#endif /* XR_BYTE_ARRAY_REPEAT_CORE_H */
