/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xslice_builtins.h - Slice type builtin method declarations
 *
 * KEY CONCEPT:
 *   ArraySlice: zero-copy mutable view into an Array.
 */

#ifndef XSLICE_BUILTINS_H
#define XSLICE_BUILTINS_H

#include "xvalue.h"
#include "xdefs.h"

/* ========== ArraySlice Methods (self = receiver ArraySlice) ========== */

XR_FUNC XrValue xr_builtin_array_slice_length(XrayIsolate *iso, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_slice_get(XrayIsolate *iso, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_slice_set(XrayIsolate *iso, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_slice_to_array(XrayIsolate *iso, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_slice_index_of(XrayIsolate *iso, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_slice_contains(XrayIsolate *iso, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_slice_first(XrayIsolate *iso, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_slice_last(XrayIsolate *iso, XrValue self, XrValue *args, int argc);

// Higher-order function methods
XR_FUNC XrValue xr_builtin_array_slice_for_each(XrayIsolate *iso, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_slice_map(XrayIsolate *iso, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_slice_filter(XrayIsolate *iso, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_slice_reduce(XrayIsolate *iso, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_slice_find(XrayIsolate *iso, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_slice_every(XrayIsolate *iso, XrValue self, XrValue *args, int argc);
XR_FUNC XrValue xr_builtin_array_slice_some(XrayIsolate *iso, XrValue self, XrValue *args, int argc);

// Register ArraySlice as native type
XR_FUNC void xr_array_slice_register_native_type(XrayIsolate *X);

#endif  // XSLICE_BUILTINS_H
