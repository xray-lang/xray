/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_array_backing.h - One fixed prefix for typed Array tails
 *
 * KEY CONCEPT:
 *   String arrays own exactly the initialized prefix. The underlying allocation
 *   still tracks its complete capacity for extent and billing.
 */
#ifndef XNATIVE_ARRAY_BACKING_H
#define XNATIVE_ARRAY_BACKING_H
#include "xnative_string_backing.h"
#include "xnative_value_plan.h"

#include "xray_native_array.h"
#include "xray_native_place.h"

/* Capacity counts elements and is derived by prepare from the actual extent.
 * Typed factories select only their setup's descriptor. Inputs remain borrowed;
 * String producers retain each element, install it, then advance length.
 * Failure retires the initialized prefix and keeps the old slot unchanged. */
XR_FUNC XrtManagedStatus xrt_array_backing_i64_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const int64_t *, size_t count, size_t capacity);
XR_FUNC XrtManagedStatus xrt_array_backing_string_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, XrtStringBacking *const *, size_t count, size_t capacity);
XR_FUNC XrtManagedStatus xrt_array_backing_u8_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const uint8_t *, size_t count, size_t capacity);
XR_FUNC XrtManagedStatus xrt_array_backing_u64_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const uint64_t *, size_t count, size_t capacity);
/* An empty Array whose allocated tail holds `capacity` rows. The capacity is
 * language-supplied rather than derived from input, so a negative request is
 * an argument failure and an unrepresentable one is an overflow failure. */
XR_FUNC XrtManagedStatus xrt_array_backing_with_capacity_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, XrtBackingArrayKind, int64_t capacity);

/* The bound Ops owns the exact inline aggregate plan and its physical schema.
 * Bind once during receiving-layout preparation, then use those Ops throughout
 * construction. Each input row has the admitted plan's size and alignment;
 * pointer fields borrow real live references for the duration of the copy. */
XR_FUNC XrtManagedStatus xrt_array_backing_inline_bind(XrtBillingOwner *,
    const XrtValuePlan *element, XrtManagedOps **empty);
XR_FUNC XrtManagedStatus xrt_array_backing_inline_create(XrtManagedFrame *, size_t,
    XrtManagedOps *, const void *rows, size_t count, size_t capacity);
/* The State boundary accepts only the exact runtime element identity. */
XR_FUNC bool xrt_array_backing_inline_matches(const XrtManagedHeader *, const XrtValuePlan *element);
/* A synchronous row borrow. The caller keeps the Array live and may publish
 * an independent result through the ordinary owned inline destination. */
XR_FUNC XrtManagedStatus xrt_array_backing_inline_at(const XrtArrayBacking *,
    const XrtValuePlan *element, int64_t index, const void **empty_borrow);
/* Appends a borrowed Class to a local slot using its verified inline Array
 * producer and a distinct empty slot in the same frame. */
XR_FUNC XrtManagedStatus xrt_array_backing_inline_class_append(XrtPlace, size_t temporary,
    XrtManagedOps *, const XrtValuePlan *element, XrtManagedHeader *item);
XR_FUNC XrtManagedStatus xrt_array_backing_growth_capacity(size_t prefix, size_t current,
    size_t required, size_t stride, size_t *output);

/* The admitted caller supplies the source's exact kind. This constructs a new
 * same-kind COW candidate and does not modify the source or any of its owners. */
XR_FUNC XrtManagedStatus xrt_array_backing_copy_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, XrtBackingArrayKind, const XrtArrayBacking *, size_t capacity);
XR_FUNC XrtManagedStatus xrt_array_backing_capacity(const XrtManagedHeader *, XrtBackingArrayKind, int64_t *);
/* Produce one owned candidate in a true-empty slot. The source remains live
 * and unchanged through both the reuse and allocation paths. */
XR_FUNC XrtManagedStatus xrt_array_backing_reserve_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, XrtBackingArrayKind, const XrtManagedHeader *, int64_t capacity);
/* Scalar bits are copied without pointer aliasing; U8 is zero extended. */
XR_FUNC XrtManagedStatus xrt_array_backing_read_bits(const XrtArrayBacking *,
    XrtBackingArrayKind, size_t, uint64_t *output);
XR_FUNC XrtManagedStatus xrt_array_backing_string_at(const XrtArrayBacking *, size_t,
    const XrtStringBacking **empty_borrow);
#endif // XNATIVE_ARRAY_BACKING_H
