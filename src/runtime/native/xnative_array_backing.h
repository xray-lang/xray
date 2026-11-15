/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_array_backing.h - One fixed prefix for four admitted Array tails
 *
 * KEY CONCEPT:
 *   Every String tail slot is a real NULL or owner; logical length never bounds
 *   the generic iterative retirement of the actual allocated capacity.
 */
#ifndef XNATIVE_ARRAY_BACKING_H
#define XNATIVE_ARRAY_BACKING_H
#include "xnative_string_backing.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200) /* C11 flexible array member */
#endif
typedef struct XrtArrayBacking {
    XrtManagedHeader managed;
    size_t length, capacity;
    unsigned char elements[];
} XrtArrayBacking;
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
_Static_assert(offsetof(XrtArrayBacking, length) == 16 &&
    offsetof(XrtArrayBacking, capacity) == 24 && offsetof(XrtArrayBacking, elements) == 32 &&
    sizeof(XrtArrayBacking) == 32 && 32 % _Alignof(uint64_t) == 0 &&
    32 % _Alignof(XrtManagedHeader *) == 0, "all four Array tails share one aligned prefix");

/* Capacity counts elements and is derived by prepare from the actual extent.
 * Typed factories select only their setup's descriptor. Inputs remain borrowed;
 * String elements are retained before installing each owned tail pointer.
 * Failure retires the full pending capacity and keeps the old slot unchanged. */
XR_FUNC XrtManagedStatus xrt_array_backing_i64_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const int64_t *, size_t count, size_t capacity);
XR_FUNC XrtManagedStatus xrt_array_backing_string_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, XrtStringBacking *const *, size_t count, size_t capacity);
XR_FUNC XrtManagedStatus xrt_array_backing_u8_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const uint8_t *, size_t count, size_t capacity);
XR_FUNC XrtManagedStatus xrt_array_backing_u64_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const uint64_t *, size_t count, size_t capacity);

/* The admitted caller supplies the source's exact kind. This constructs a new
 * same-kind COW candidate and does not modify the source or any of its owners. */
XR_FUNC XrtManagedStatus xrt_array_backing_copy_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, XrtBackingArrayKind, const XrtArrayBacking *, size_t capacity);
/* Scalar bits are copied without pointer aliasing; U8 is zero extended. */
XR_FUNC XrtManagedStatus xrt_array_backing_read_bits(const XrtArrayBacking *,
    XrtBackingArrayKind, size_t, uint64_t *output);
XR_FUNC XrtManagedStatus xrt_array_backing_string_at(const XrtArrayBacking *, size_t,
    const XrtStringBacking **empty_borrow);
#endif // XNATIVE_ARRAY_BACKING_H
