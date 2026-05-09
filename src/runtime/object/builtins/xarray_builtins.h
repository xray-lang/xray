/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarray_builtins.h - Array built-in methods
 *
 * KEY CONCEPT:
 *   Global Array constructor and static methods.
 */

#ifndef XARRAY_BUILTINS_H
#define XARRAY_BUILTINS_H

#include "xvalue.h"
#include "xdefs.h"

// Global Array constructor (self = class value, ignored)
XR_FUNC XrValue xr_builtin_array_construct(XrayIsolate *isolate, XrValue self, XrValue *args,
                                           int argc);

// Array static methods (self = class value, ignored)
XR_FUNC XrValue xr_builtin_array_from(XrayIsolate *isolate, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_range(XrayIsolate *isolate, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_with_capacity(XrayIsolate *isolate, XrValue self, XrValue *args,
                                               int argc);

#endif  // XARRAY_BUILTINS_H
