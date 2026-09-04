/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * crypto.c - Private cryptographic provider leaves
 *
 * KEY CONCEPT:
 *   Public cryptographic algorithms live in crypto.xr. This translation unit
 *   contains only the two private provider operations that Xray cannot state:
 *   platform entropy and constant-time byte comparison.
 */

#include "crypto.h"
#include "../../src/shared/xr_crypto_core.h"
#include "../../stdlib/common.h"
#include "../../src/os/os_random.h"
#include "../../src/runtime/mem/xheap.h"
#include "../../src/runtime/value/xvalue.h"

/* ========== Module Bindings ========== */

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
