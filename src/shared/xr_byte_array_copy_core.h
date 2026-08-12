/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_byte_array_copy_core.h - Canonical Array<byte> bounded copy semantics.
 */

#ifndef XR_BYTE_ARRAY_COPY_CORE_H
#define XR_BYTE_ARRAY_COPY_CORE_H

#if !defined(XR_BYTE_ARRAY_COPY_C90)
#include "xr_elem_type.h"
#include "xr_semantic_owner_ids_gen.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define XR_BYTE_ARRAY_COPY_INLINE static inline
#else
/* The restricted C90 runtime supplies fixed-width integers, bool, size_t,
 * XR_ELEM_U8, and memmove before including this semantic core. */
#define XR_BYTE_ARRAY_COPY_INLINE static
#endif

typedef enum XrByteArrayCopyKind {
    XR_BYTE_ARRAY_COPY_WITHIN = 0,
    XR_BYTE_ARRAY_COPY_FROM = 1
} XrByteArrayCopyKind;

typedef enum XrByteArrayCopyStatus {
    XR_BYTE_ARRAY_COPY_OK = 0,
    XR_BYTE_ARRAY_COPY_INVALID_KIND = 1,
    XR_BYTE_ARRAY_COPY_WRONG_ELEMENT_TYPE = 2,
    XR_BYTE_ARRAY_COPY_OUT_OF_BOUNDS = 3,
    XR_BYTE_ARRAY_COPY_NO_DATA = 4
} XrByteArrayCopyStatus;

typedef struct XrByteArrayCopyResult {
    XrByteArrayCopyStatus status;
    bool changed;
} XrByteArrayCopyResult;

#if !defined(XR_BYTE_ARRAY_COPY_C90)
#define XR_BYTE_ARRAY_COPY_OWNER_GUARD(owner_hi, owner_lo)                                       \
    ((void) sizeof(struct {                                                                        \
        unsigned int owner_id_must_be_shared_byte_array_copy                                    \
            : (((uint64_t) (owner_hi) == XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_HI &&           \
                (uint64_t) (owner_lo) == XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_LO)              \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_BYTE_ARRAY_COPY_CONSUMER_GUARD(consumer_bit)                                          \
    ((void) sizeof(struct {                                                                        \
        unsigned int consumer_must_be_declared_for_shared_byte_array_copy                       \
            : (((uint32_t) (consumer_bit) != 0 &&                                                 \
                (((uint32_t) (consumer_bit) & ((uint32_t) (consumer_bit) - 1)) == 0) &&           \
                (XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_CONSUMERS &                              \
                 (uint32_t) (consumer_bit)) != 0)                                                 \
                   ? 1                                                                            \
                   : -1);                                                                         \
    }))

#define XR_BYTE_ARRAY_COPY_OWNER_APPLY(owner_hi, owner_lo, consumer_bit, expression)             \
    (XR_BYTE_ARRAY_COPY_OWNER_GUARD((owner_hi), (owner_lo)),                                     \
     XR_BYTE_ARRAY_COPY_CONSUMER_GUARD((consumer_bit)), (expression))
#endif

XR_BYTE_ARRAY_COPY_INLINE bool xr_byte_array_copy_range_ok(int64_t length, int64_t offset,
                                                            int64_t count) {
    return length >= 0 && offset >= 0 && count >= 0 && offset <= length && count <= length - offset;
}

XR_BYTE_ARRAY_COPY_INLINE XrByteArrayCopyResult xr_byte_array_copy_core(
    XrByteArrayCopyKind kind, void *dst_data, int64_t dst_length, uint8_t dst_elem_type,
    const void *src_data, int64_t src_length, uint8_t src_elem_type, int64_t src_offset,
    int64_t dst_offset, int64_t count) {
    XrByteArrayCopyResult result;
    result.status = XR_BYTE_ARRAY_COPY_OK;
    result.changed = false;

    if (kind != XR_BYTE_ARRAY_COPY_WITHIN && kind != XR_BYTE_ARRAY_COPY_FROM) {
        result.status = XR_BYTE_ARRAY_COPY_INVALID_KIND;
        return result;
    }
    if (dst_elem_type != XR_ELEM_U8 || src_elem_type != XR_ELEM_U8) {
        result.status = XR_BYTE_ARRAY_COPY_WRONG_ELEMENT_TYPE;
        return result;
    }
    if (!xr_byte_array_copy_range_ok(src_length, src_offset, count) ||
        !xr_byte_array_copy_range_ok(dst_length, dst_offset, count)) {
        result.status = XR_BYTE_ARRAY_COPY_OUT_OF_BOUNDS;
        return result;
    }
    if (count == 0)
        return result;
    if (!dst_data || !src_data) {
        result.status = XR_BYTE_ARRAY_COPY_NO_DATA;
        return result;
    }

    memmove((uint8_t *) dst_data + dst_offset, (const uint8_t *) src_data + src_offset,
            (size_t) count);
    result.changed = true;
    return result;
}

#undef XR_BYTE_ARRAY_COPY_INLINE

#endif /* XR_BYTE_ARRAY_COPY_CORE_H */
