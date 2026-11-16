/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_array_slice.h - Typed materialization from protected Array windows
 */
#ifndef XNATIVE_ARRAY_SLICE_H
#define XNATIVE_ARRAY_SLICE_H
#include "xray_native_array_slice.h"
#include "xnative_array_backing.h"
/* Source protection precedes every allocating factory callback. The target is
 * an ordinary empty Array pending slot, never an owning Slice or a heap View. */
XR_FUNC XrtManagedStatus xrt_array_slice_copy_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, XrtBackingArrayKind, const XrtReadonlyArraySlice *);
#endif // XNATIVE_ARRAY_SLICE_H
