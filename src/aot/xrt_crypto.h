/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_crypto.h - Freestanding AOT crypto helpers
 */

#ifndef XRT_CRYPTO_H
#define XRT_CRYPTO_H

#include "../os/os_random.h"
#include "../shared/xr_crypto_core.h"
#include "xrt_value.h"

static inline XrValue xrt_crypto_fill_random_bytes(XrValue bytes_value) {
    if (!XR_IS_ARRAY(bytes_value) || !bytes_value.ptr)
        return XR_NULL_VAL;
    xrt_array_t *bytes = (xrt_array_t *) bytes_value.ptr;
    if (bytes->elem_type != XR_ELEM_U8 || bytes->elem_size != 1 || bytes->length < 0 ||
        (bytes->length != 0 && !bytes->data))
        return XR_NULL_VAL;
    if (bytes->length != 0)
        xr_random_bytes((uint8_t *) bytes->data, (size_t) bytes->length);
    return XR_NULL_VAL;
}

static inline XrValue xrt_crypto_timing_safe_equal_bytes(XrValue a_value, XrValue b_value) {
    if (!XR_IS_ARRAY(a_value) || !XR_IS_ARRAY(b_value))
        return XR_FALSE_VAL;
    const xrt_array_t *a = (const xrt_array_t *) a_value.ptr;
    const xrt_array_t *b = (const xrt_array_t *) b_value.ptr;
    if (!a || !b || a->elem_type != XR_ELEM_U8 || b->elem_type != XR_ELEM_U8 ||
        a->elem_size != 1 || b->elem_size != 1 || a->length < 0 || b->length < 0 ||
        (a->length != 0 && !a->data) || (b->length != 0 && !b->data))
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(xr_crypto_core_timing_safe_equal(
        (const char *) a->data, (size_t) a->length, (const char *) b->data, (size_t) b->length));
}

#endif  // XRT_CRYPTO_H
