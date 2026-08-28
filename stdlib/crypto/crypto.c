/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * crypto.c - Cryptographic functions implementation
 *
 * KEY CONCEPT:
 *   Pure C implementation of cryptographic primitives. No external library
 *   dependencies - all algorithms (MD5, SHA-1, SHA-256, HMAC, AES) are
 *   implemented from scratch for portability.
 */

#include "crypto.h"
#include "../../src/shared/xr_crypto_core.h"
#include "../../src/base/xchecks.h"
#include "../../src/base/xmalloc.h"
#ifndef XR_CRYPTO_CORE_ONLY
#include "../../stdlib/common.h"
#include "../../src/os/os_random.h"
#include "../../src/base/xglobal_indices.h"
#include "../../src/runtime/class/xbuiltin_enum_error.h"
#include "../../src/runtime/core/xr_runtime_core.h"
#include "../../src/runtime/mem/xalloc_unified.h"
#include "../../src/runtime/object/xpanic_info.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/vm/xvm.h"
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ========== Utility Macros ========== */

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROTL64(x, n) (((x) << (n)) | ((x) >> (64 - (n))))
#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

/* ========== Secure Memory Wipe ========== */

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

/* ========== HMAC Implementation ========== */

typedef void (*HashFn)(const uint8_t *data, size_t len, uint8_t *digest);

/*
 * Generic HMAC computation using function pointers.
 * block_size: hash block size (64 for MD5/SHA1/SHA256, 128 for SHA512)
 * digest_size: hash output size in bytes
 */
static void hmac_compute(HashFn hash, int block_size, int digest_size, const uint8_t *key,
                         size_t key_len, const uint8_t *data, size_t data_len, uint8_t *digest) {
    uint8_t k[128] = {0};
    uint8_t ipad[128], opad[128];
    uint8_t inner[64];

    if ((int) key_len > block_size) {
        hash(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }

    for (int i = 0; i < block_size; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    size_t inner_len = (size_t) block_size + data_len;
    size_t outer_len = (size_t) block_size + (size_t) digest_size;

    uint8_t stack_buf[4096];
    uint8_t *inner_buf =
        (inner_len <= sizeof(stack_buf)) ? stack_buf : (uint8_t *) xr_malloc(inner_len);
    if (!inner_buf) {
        memset(digest, 0, digest_size);
        return;
    }

    memcpy(inner_buf, ipad, block_size);
    memcpy(inner_buf + block_size, data, data_len);
    hash(inner_buf, inner_len, inner);

    if (inner_buf != stack_buf)
        xr_free(inner_buf);

    // opad || inner_hash (always fits in stack)
    uint8_t outer_buf[192];  // 128 + 64 max
    memcpy(outer_buf, opad, block_size);
    memcpy(outer_buf + block_size, inner, digest_size);
    hash(outer_buf, outer_len, digest);

    xr_secure_wipe(k, sizeof(k));
    xr_secure_wipe(ipad, sizeof(ipad));
    xr_secure_wipe(opad, sizeof(opad));
    xr_secure_wipe(inner, sizeof(inner));
    xr_secure_wipe(outer_buf, sizeof(outer_buf));
}

static void hash_sha256_wrapper(const uint8_t *data, size_t len, uint8_t *digest) {
    xr_sha256(data, len, digest);
}
XR_FUNC void xr_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data,
                            size_t data_len, uint8_t digest[32]) {
    hmac_compute(hash_sha256_wrapper, 64, 32, key, key_len, data, data_len, digest);
}

/* ========== Utility Functions ========== */

static const char hex_chars[] = "0123456789abcdef";

XR_FUNC void xr_bytes_to_hex(const uint8_t *bytes, size_t len, char *output) {
    XR_DCHECK(bytes != NULL || len == 0, "xr_bytes_to_hex: NULL bytes");
    XR_DCHECK(output != NULL, "xr_bytes_to_hex: NULL output");
    for (size_t i = 0; i < len; i++) {
        output[i * 2] = hex_chars[(bytes[i] >> 4) & 0xF];
        output[i * 2 + 1] = hex_chars[bytes[i] & 0xF];
    }
    output[len * 2] = '\0';
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

#ifndef XR_CRYPTO_CORE_ONLY

/* ========== Module Bindings ========== */

static void crypto_publish_builtin_enum_error(XrVMRuntime *iso, int builtin_index,
                                              uint32_t member_index, const char *fallback_message) {
    XrBuiltinEnumErrorResult result = xr_builtin_enum_error_construct(
        iso ? xr_isolate_get_runtime_core(iso) : NULL, builtin_index, member_index);
    if (result.status == XR_BUILTIN_ENUM_ERROR_OK) {
        XrValue error = result.value;
        XrExecutionErrorPublishStatus publish = xr_exec_context_publish_error_owned(
            iso ? xr_isolate_get_runtime_core(iso) : NULL, &error);
        if (publish == XR_EXEC_ERROR_PUBLISH_OK)
            return;
        xr_rc_release_value(xr_current_coro_heap(), error);
        error = xr_null();
        if (publish == XR_EXEC_ERROR_PUBLISH_CHANNEL_OCCUPIED)
            return;
    }
    XrValue exc = xr_panic_info_newf(iso, XR_ERR_INTERNAL, "%s",
                                     fallback_message ? fallback_message
                                                      : "failed to construct typed crypto error");
    xr_vm_throw_exception(iso, exc);
}

/* ========== Module-private native leaves ========== */

// crypto.__randomBytes(n) -> Array<u8>
//
// The ceiling on n is the module's policy and lives in crypto.xr; this answers
// whatever length it is asked for so the boundary states one fact only.
static XrValue crypto_random_bytes_raw(XrVMRuntime *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_INT(args[0]))
        return xr_null();
    int64_t n = XR_TO_INT(args[0]);
    if (n <= 0 || n > INT32_MAX)
        return xr_null();
    XrArray *arr = xr_byte_array_new(xr_current_coro(isolate), (int32_t) n);
    if (!arr)
        return xr_null();
    xr_random_bytes(arr->data, (size_t) n);
    arr->length = (int32_t) n;
    return xr_value_from_array(arr);
}

// crypto.__timingSafeEqualBytes(a, b) -> bool
//
// Kept native because the comparison's cost must not depend on where the two
// buffers first differ, and no Xray construct states that an optimizer has to
// preserve the whole loop.
static XrValue crypto_timing_safe_equal_bytes(XrVMRuntime *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 2 || !xr_value_is_array(args[0]) || !xr_value_is_array(args[1]))
        return xr_bool(false);
    XrArray *a = xr_value_to_array(args[0]);
    XrArray *b = xr_value_to_array(args[1]);
    if (!a || !b || a->elem_type != XR_ELEM_U8 || b->elem_type != XR_ELEM_U8)
        return xr_bool(false);
    return xr_bool(xr_crypto_core_timing_safe_equal((const char *) a->data, (size_t) a->length,
                                                    (const char *) b->data, (size_t) b->length));
}

#define XR_STDLIB_VM_BIND_MODULE_CRYPTO 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_CRYPTO

XR_FUNC XrModule *xr_native_module_create_crypto(XrVMRuntime *isolate) {
    XrModule *mod = xr_module_create_native(isolate, "crypto");
    if (!mod)
        return NULL;

    xr_stdlib_vm_bind_crypto_generated(isolate, mod);

    return mod;
}

#endif /* XR_CRYPTO_CORE_ONLY */
