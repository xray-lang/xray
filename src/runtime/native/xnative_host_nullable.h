/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_nullable.h - Preflight and publish a one-owner host result
 */
#ifndef XNATIVE_HOST_NULLABLE_H
#define XNATIVE_HOST_NULLABLE_H
#include "base/xdefs.h"
#include "xray_native_place.h"

typedef struct XrtHostNullableOwner {
    uint8_t present;
    XrtManagedHeader *owner;
} XrtHostNullableOwner;

typedef struct XrtHostNullableSlots {
    XrtManagedFrame *frame;
    size_t destination, temporary, offset, temporary_owner_offset;
} XrtHostNullableSlots;

typedef struct XrtHostNullablePlaces {
    XrtPlace destination, temporary;
    const XrtManagedHeader *current;
    uint8_t *tag;
} XrtHostNullablePlaces;

/* An active root also proves the staging offset and the old presence tag. */
XR_FUNC XrtManagedStatus xrt_host_nullable_prepare(const XrtHostNullableSlots *, XrtManagedRoot *,
    XrtHostNullablePlaces *);
XR_FUNC XrtManagedStatus xrt_host_nullable_publish(const XrtHostNullablePlaces *, bool present);
#endif // XNATIVE_HOST_NULLABLE_H
