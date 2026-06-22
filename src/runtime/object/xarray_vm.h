/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarray_vm.h - VM-facing Array adapters
 */

#ifndef XARRAY_VM_H
#define XARRAY_VM_H

#include "xarray.h"

struct XrayIsolate;
struct XrClosure;
struct XrString;

XR_FUNC XrArray *xr_array_new_shared(struct XrayIsolate *X, int capacity);
XR_FUNC void xr_array_foreach(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC XrArray *xr_array_map(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC XrArray *xr_array_filter(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC XrValue xr_array_reduce(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback,
                                XrValue initial);
XR_FUNC XrValue xr_array_find(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC int xr_array_find_index(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC bool xr_array_every(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC bool xr_array_some(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback);
XR_FUNC void xr_array_sort(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *comparator);
XR_FUNC struct XrString *xr_array_join(struct XrayIsolate *iso, XrArray *arr,
                                       struct XrString *delimiter);
XR_FUNC struct XrString *xr_array_to_string(struct XrayIsolate *iso, XrArray *arr);

#endif  // XARRAY_VM_H
