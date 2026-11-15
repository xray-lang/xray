/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_string_backing.h - Fixed UTF-8 bytes under real managed ownership
 *
 * KEY CONCEPT:
 *   Capacity is the immutable allocated tail extent, including its final NUL.
 */
#ifndef XNATIVE_STRING_BACKING_H
#define XNATIVE_STRING_BACKING_H
#include "xnative_backing_types.h"

#include "xray_native_string_value.h"

/* Reserve counts tail bytes, including the terminator; zero selects length+1.
 * Inputs are synchronous borrows. The stable pending slot exists before any
 * payload write. Failure aborts only this factory's pending token and preserves
 * the old committed owner. Success leaves one complete owner in the slot. */
XR_FUNC XrtManagedStatus xrt_string_backing_create(XrtManagedFrame *, size_t slot,
    const XrtBackingTypes *, const char *bytes, size_t length, size_t reserve);
XR_FUNC XrtManagedStatus xrt_string_backing_concat_create(XrtManagedFrame *, size_t slot,
    const XrtBackingTypes *, const XrtStringBacking *, const XrtStringBacking *, size_t reserve);
#endif // XNATIVE_STRING_BACKING_H
