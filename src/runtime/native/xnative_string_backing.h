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

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200) /* C11 flexible array member */
#endif
typedef struct XrtStringBacking {
    XrtManagedHeader managed;
    size_t length, runes, capacity;
    char bytes[];
} XrtStringBacking;
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
_Static_assert(sizeof(size_t) == 8 && offsetof(XrtStringBacking, length) == 16 &&
    offsetof(XrtStringBacking, runes) == 24 && offsetof(XrtStringBacking, capacity) == 32 &&
    offsetof(XrtStringBacking, bytes) == 40 && sizeof(XrtStringBacking) == 40,
    "String backing has a fixed forty-byte prefix");

/* Reserve counts tail bytes, including the terminator; zero selects length+1.
 * Inputs are synchronous borrows. The stable pending slot exists before any
 * payload write. Failure aborts only this factory's pending token and preserves
 * the old committed owner. Success leaves one complete owner in the slot. */
XR_FUNC XrtManagedStatus xrt_string_backing_create(XrtManagedFrame *, size_t slot,
    const XrtBackingTypes *, const char *bytes, size_t length, size_t reserve);
XR_FUNC XrtManagedStatus xrt_string_backing_concat_create(XrtManagedFrame *, size_t slot,
    const XrtBackingTypes *, const XrtStringBacking *, const XrtStringBacking *, size_t reserve);
#endif // XNATIVE_STRING_BACKING_H
