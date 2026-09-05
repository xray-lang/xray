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
#include "../../stdlib/common.h"
#include "../../src/os/os_random.h"
#include "../../src/runtime/value/xvalue.h"

/* ========== Module Bindings ========== */

/* ========== Module-private native leaves ========== */

// crypto.__fillRandomBytes(bytes) -> ()
//
// Allocation and length policy live in crypto.xr. This leaf only asks the
// platform provider to overwrite the destination supplied by Xray.
static XrValue crypto_fill_random_bytes(XrVMRuntime *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !xr_value_is_array(args[0]))
        return xr_null();
    XrArray *bytes = xr_value_to_array(args[0]);
    if (!bytes || bytes->elem_type != XR_ELEM_U8 || bytes->length < 0 ||
        (bytes->length != 0 && !bytes->data))
        return xr_null();
    (void) isolate;
    if (bytes->length != 0)
        xr_random_bytes(bytes->data, (size_t) bytes->length);
    return xr_null();
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
    if (!a || !b || a->elem_type != XR_ELEM_U8 || b->elem_type != XR_ELEM_U8 || a->length < 0 ||
        b->length < 0 || (a->length != 0 && !a->data) || (b->length != 0 && !b->data))
        return xr_bool(false);
    return xr_bool(xr_crypto_core_timing_safe_equal((const char *) a->data, (size_t) a->length,
                                                    (const char *) b->data, (size_t) b->length));
}

#define XR_STDLIB_VM_BIND_MODULE_CRYPTO 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_CRYPTO
