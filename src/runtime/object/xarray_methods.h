/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarray_methods.h - Array instance method dispatch table.
 *
 * KEY POINTS:
 *   - Owns the per-type XrMethodSlot table for arrays/slices.
 *     OP_INVOKE_BUILTIN, OP_INVOKE invoke_array, the JIT array hint,
 *     and the cold-path go-statement helper all reach methods
 *     through xr_method_table_lookup(XR_TID_ARRAY, ...).
 *   - The bound-method bridge (xr_array_get_handler in
 *     runtime/closure/xbound_method.c) pulls from the same table —
 *     no duplicate handler registry.
 *   - Method bodies are `static` inside xarray_methods.c.
 */

#ifndef XARRAY_METHODS_H
#define XARRAY_METHODS_H

#include "../value/xmethod_table.h"
#include "../symbol/xsymbol_table.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const XrMethodSlot xr_array_method_table[SYMBOL_BUILTIN_COUNT];

/* Register Array methods on an XrClass via native_type_classes[XR_TARRAY].
 * Called once during isolate init. */
struct XrayIsolate;
XR_FUNC void xr_array_register_native_type(struct XrayIsolate *isolate);

#ifdef __cplusplus
}
#endif

#endif /* XARRAY_METHODS_H */
