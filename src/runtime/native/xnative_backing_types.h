/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_backing_types.h - Stable private descriptors for fixed value storage
 *
 * KEY CONCEPT:
 *   A receiving setup owns immutable descriptors; objects keep their own
 *   descriptor leases after the move-only setup has been dropped.
 */
#ifndef XNATIVE_BACKING_TYPES_H
#define XNATIVE_BACKING_TYPES_H
#include "xnative_managed_root.h"

typedef enum XrtBackingArrayKind {
    XRT_BACKING_ARRAY_I64, XRT_BACKING_ARRAY_STRING,
    XRT_BACKING_ARRAY_U8, XRT_BACKING_ARRAY_U64, XRT_BACKING_ARRAY_COUNT
} XrtBackingArrayKind;

static inline size_t xrt_backing_array_stride(XrtBackingArrayKind kind) {
    static const size_t strides[] = {sizeof(int64_t), sizeof(XrtManagedHeader *), sizeof(uint8_t), sizeof(uint64_t)};
    return (unsigned)kind < XRT_BACKING_ARRAY_COUNT ? strides[kind] : 0;
}

/* Zero initialization is empty. Only init/drop write these owned handles;
 * copying a live setup would duplicate owners without retaining them. */
typedef struct XrtBackingTypes {
    XrtManagedOps *string;
    XrtManagedOps *arrays[XRT_BACKING_ARRAY_COUNT];
    XrtManagedOps *map;
} XrtBackingTypes;

/* Cold setup only. Failure leaves the empty output unchanged and refunds every
 * candidate Plan/Ops. Factories borrow this setup and never bind descriptors. */
XR_FUNC XrtManagedStatus xrt_backing_types_init(XrtBillingOwner *, XrtBackingTypes *empty);
XR_FUNC void xrt_backing_types_drop(XrtBackingTypes *);

/* Copy one real reference into an entirely empty stable slot. Failed adoption
 * releases only the extra owner; no payload allocation or owner lookup occurs.
 * The input is a real managed borrow, never an arbitrary pointer to validate. */
XR_FUNC XrtManagedStatus xrt_backing_slot_copy(XrtManagedFrame *, size_t,
    const XrtManagedHeader *borrowed);
#endif // XNATIVE_BACKING_TYPES_H
