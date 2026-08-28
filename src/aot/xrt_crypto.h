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

static inline XrValue xrt_crypto_encrypt(const char *key, int64_t key_len, const char *plain,
                                         int64_t plain_len) {
    if ((!key && key_len != 0) || (!plain && plain_len != 0) || key_len < 0 || plain_len < 0)
        return XR_NULL_VAL;

    size_t input_len = (size_t) plain_len;
    size_t padded_len = 0;
    size_t hex_len = 0;
    if (!xr_crypto_core_aead_encrypt_plan(input_len, &padded_len, &hex_len))
        return XR_NULL_VAL;
    size_t raw_len = XR_AEAD_OVERHEAD + padded_len;  // IV + ciphertext + tag

    uint8_t iv[16];
    xr_random_bytes(iv, 16);

    uint8_t stack_plain[4096];
    uint8_t *padded =
        (padded_len <= sizeof(stack_plain)) ? stack_plain : (uint8_t *) XRT_MALLOC(padded_len);
    if (!padded)
        return XR_NULL_VAL;

    uint8_t stack_raw[4096];
    uint8_t *raw = (raw_len <= sizeof(stack_raw)) ? stack_raw : (uint8_t *) XRT_MALLOC(raw_len);
    if (!raw) {
        if (padded != stack_plain)
            XRT_FREE(padded);
        return XR_NULL_VAL;
    }

    XrValue result = xrt_str_alloc(hex_len);
    char *hex = xr_str_buf(result);
    if (!xr_crypto_core_aead_encrypt_hex((const uint8_t *) key, (size_t) key_len,
                                         (const uint8_t *) plain, input_len, iv, padded, padded_len,
                                         raw, raw_len, hex, hex_len + 1)) {
        xrt_release(result);
        if (padded != stack_plain)
            XRT_FREE(padded);
        if (raw != stack_raw)
            XRT_FREE(raw);
        return XR_NULL_VAL;
    }

    if (padded != stack_plain)
        XRT_FREE(padded);
    if (raw != stack_raw)
        XRT_FREE(raw);
    return result;
}

static inline XrValue xrt_crypto_decrypt(const char *key, int64_t key_len, const char *cipher_hex,
                                         int64_t cipher_hex_len) {
    if ((!key && key_len != 0) || (!cipher_hex && cipher_hex_len != 0) || key_len < 0 ||
        cipher_hex_len < 0)
        return XR_NULL_VAL;

    size_t raw_len = 0;
    size_t cipher_len = 0;
    if (!xr_crypto_core_aead_decrypt_plan((size_t) cipher_hex_len, &raw_len, &cipher_len)) {
        xrt_crypto_set_builtin_enum_error("CryptoError", "InvalidLength", 0);
        return XR_NULL_VAL;
    }

    uint8_t stack_raw[4096];
    uint8_t *raw = (raw_len <= sizeof(stack_raw)) ? stack_raw : (uint8_t *) XRT_MALLOC(raw_len);
    if (!raw)
        return XR_NULL_VAL;

    uint8_t stack_plain[4096];
    uint8_t *plain =
        (cipher_len <= sizeof(stack_plain)) ? stack_plain : (uint8_t *) XRT_MALLOC(cipher_len);
    if (!plain) {
        if (raw != stack_raw)
            XRT_FREE(raw);
        return XR_NULL_VAL;
    }

    XrValue result = XR_NULL_VAL;
    size_t plain_out_len = 0;
    if (xr_crypto_core_aead_decrypt_hex((const uint8_t *) key, (size_t) key_len, cipher_hex,
                                        (size_t) cipher_hex_len, raw, raw_len, plain, cipher_len,
                                        &plain_out_len)) {
        result = xrt_str_alloc(plain_out_len);
        memcpy(xr_str_buf(result), plain, plain_out_len);
        xr_str_buf(result)[plain_out_len] = '\0';
    }

    if (raw != stack_raw)
        XRT_FREE(raw);
    if (plain != stack_plain)
        XRT_FREE(plain);
    return result;
}

#endif  // XRT_CRYPTO_H
