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

#include <string.h>

#include "../os/os_random.h"
#include "../shared/xr_crypto_core.h"
#include "xrt_arc.h"
#include "xrt_value.h"

extern XR_THREAD_LOCAL XrValue xrt_pending_error;

static inline void xrt_crypto_set_builtin_enum_error(const char *enum_name, const char *member_name,
                                                     uint32_t member_index) {
    XrAotEnumAggregate err =
        xrt_enum_aggregate_make(0, (int64_t) member_index, 0, enum_name, member_name, NULL);
    xrt_pending_error = xrt_enum_aggregate_box(err);
}

static inline XrValue xrt_crypto_hex_result(const uint8_t *digest, size_t digest_len) {
    if (!digest || digest_len > 64)
        return XR_NULL_VAL;
    size_t hex_len = digest_len * 2;
    char hex[129];
    if (!xr_crypto_core_digest_hex(digest, digest_len, hex, sizeof(hex)))
        return XR_NULL_VAL;
    XrValue result = xrt_str_alloc(hex_len);
    memcpy(xr_str_buf(result), hex, hex_len);
    xr_str_buf(result)[hex_len] = '\0';
    return result;
}

static inline XrValue xrt_crypto_random_bytes_raw(XrValue len_value) {
    if (!XR_IS_INT(len_value))
        return XR_NULL_VAL;
    int64_t n = XR_TO_INT(len_value);
    if (n <= 0 || n > INT32_MAX)
        return XR_NULL_VAL;
    XrValue bytes_value = xrt_array_new_typed_uninit(n, XR_ELEM_U8);
    xrt_array_t *bytes = (xrt_array_t *) bytes_value.ptr;
    if (!bytes)
        return XR_NULL_VAL;
    xr_random_bytes((uint8_t *) bytes->data, (size_t) n);
    bytes->length = n;
    return bytes_value;
}

static inline XrValue xrt_crypto_timing_safe_equal_bytes(XrValue a_value, XrValue b_value) {
    const xrt_array_t *a = (const xrt_array_t *) a_value.ptr;
    const xrt_array_t *b = (const xrt_array_t *) b_value.ptr;
    if (!a || !b)
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(xr_crypto_core_timing_safe_equal(
        (const char *) a->data, (size_t) a->length, (const char *) b->data, (size_t) b->length));
}

#endif  // XRT_CRYPTO_H
