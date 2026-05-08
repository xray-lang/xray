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
 *   - Dispatch is via native_type_classes[XR_TARRAY], registered
 *     during isolate init by xr_array_register_native_type().
 *   - Method bodies are `static` inside xarray_methods.c.
 */

#ifndef XARRAY_METHODS_H
#define XARRAY_METHODS_H

#include "../../base/xdefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register Array methods on an XrClass via native_type_classes[XR_TARRAY].
 * Called once during isolate init. */
struct XrayIsolate;
XR_FUNC void xr_array_register_native_type(struct XrayIsolate *isolate);

#ifdef __cplusplus
}
#endif

#endif /* XARRAY_METHODS_H */
