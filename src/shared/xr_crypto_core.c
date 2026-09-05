/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_crypto_core.c - Runtime and toolchain cryptographic primitives.
 */

#include "xr_crypto_core.h"

#include "../base/xchecks.h"
#include "../base/xmalloc.h"

#include <string.h>

XR_FUNC void xr_secure_wipe(void *ptr, size_t len) {
#if defined(XR_OS_MACOS)
    memset_s(ptr, len, 0, len);
#elif defined(__GLIBC__)
    explicit_bzero(ptr, len);
#else
    volatile uint8_t *p = (volatile uint8_t *) ptr;
    while (len--)
        *p++ = 0;
#endif
}

typedef void (*XrCryptoHashFn)(const uint8_t *data, size_t len, uint8_t *digest);

static void xr_crypto_hmac_compute(XrCryptoHashFn hash, size_t block_size, size_t digest_size,
                                   const uint8_t *key, size_t key_len, const uint8_t *data,
                                   size_t data_len, uint8_t *digest) {
    XR_DCHECK(hash != NULL, "xr_crypto_hmac_compute: hash must not be NULL");
    XR_DCHECK(block_size <= 128, "xr_crypto_hmac_compute: block size exceeds scratch capacity");
    XR_DCHECK(digest_size <= 64, "xr_crypto_hmac_compute: digest size exceeds scratch capacity");
    XR_DCHECK(key != NULL || key_len == 0, "xr_crypto_hmac_compute: key must be present");
    XR_DCHECK(data != NULL || data_len == 0, "xr_crypto_hmac_compute: data must be present");
    XR_DCHECK(digest != NULL, "xr_crypto_hmac_compute: digest must not be NULL");

    uint8_t normalized_key[128] = {0};
    uint8_t inner_pad[128];
    uint8_t outer_pad[128];
    uint8_t inner_digest[64];

    if (key_len > block_size)
        hash(key, key_len, normalized_key);
    else if (key_len != 0)
        memcpy(normalized_key, key, key_len);

    for (size_t i = 0; i < block_size; i++) {
        inner_pad[i] = normalized_key[i] ^ UINT8_C(0x36);
        outer_pad[i] = normalized_key[i] ^ UINT8_C(0x5c);
    }

    size_t inner_len = block_size + data_len;
    uint8_t stack_inner[4096];
    uint8_t *inner = inner_len <= sizeof(stack_inner) ? stack_inner : xr_malloc(inner_len);
    if (!inner) {
        memset(digest, 0, digest_size);
        return;
    }

    memcpy(inner, inner_pad, block_size);
    if (data_len != 0)
        memcpy(inner + block_size, data, data_len);
    hash(inner, inner_len, inner_digest);
    if (inner != stack_inner)
        xr_free(inner);

    uint8_t outer[192];
    memcpy(outer, outer_pad, block_size);
    memcpy(outer + block_size, inner_digest, digest_size);
    hash(outer, block_size + digest_size, digest);

    xr_secure_wipe(normalized_key, sizeof(normalized_key));
    xr_secure_wipe(inner_pad, sizeof(inner_pad));
    xr_secure_wipe(outer_pad, sizeof(outer_pad));
    xr_secure_wipe(inner_digest, sizeof(inner_digest));
    xr_secure_wipe(outer, sizeof(outer));
}

static void xr_crypto_sha256_adapter(const uint8_t *data, size_t len, uint8_t *digest) {
    xr_sha256(data, len, digest);
}

XR_FUNC void xr_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data,
                            size_t data_len, uint8_t digest[32]) {
    xr_crypto_hmac_compute(xr_crypto_sha256_adapter, 64, 32, key, key_len, data, data_len, digest);
}

XR_FUNC void xr_bytes_to_hex(const uint8_t *bytes, size_t len, char *output) {
    static const char hex_digits[] = "0123456789abcdef";
    XR_DCHECK(bytes != NULL || len == 0, "xr_bytes_to_hex: bytes must be present");
    XR_DCHECK(output != NULL, "xr_bytes_to_hex: output must not be NULL");
    for (size_t i = 0; i < len; i++) {
        output[i * 2] = hex_digits[(bytes[i] >> 4) & 0x0f];
        output[i * 2 + 1] = hex_digits[bytes[i] & 0x0f];
    }
    output[len * 2] = '\0';
}
