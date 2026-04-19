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


/* ========== ArraySlice Methods ========== */

XR_FUNC XrValue xr_builtin_array_slice_length(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_array_slice_get(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_array_slice_set(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_array_slice_to_array(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_array_slice_index_of(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_array_slice_contains(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_array_slice_first(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_array_slice_last(XrayIsolate *isolate, XrValue *args, int nargs);

// Higher-order function methods
XR_FUNC XrValue xr_builtin_array_slice_for_each(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_array_slice_map(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_array_slice_filter(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_array_slice_reduce(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_array_slice_find(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_array_slice_every(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_array_slice_some(XrayIsolate *isolate, XrValue *args, int nargs);

// Create slice classes (using XrClassBuilder)
typedef struct XrClass XrClass;
XR_FUNC XrClass* xr_array_slice_create_class(XrayIsolate *X, XrClass *objectClass);

#endif // XSLICE_BUILTINS_H
