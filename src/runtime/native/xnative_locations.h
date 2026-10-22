/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_locations.h - Immutable locations with explicit retained lifetimes
 *
 * KEY CONCEPT:
 *   Every path byte belongs to the owner. A borrowed view remains valid only
 *   while its caller holds a reference; retaining never copies or allocates.
 */
#ifndef XNATIVE_LOCATIONS_H
#define XNATIVE_LOCATIONS_H
#include "base/xdefs.h"
#include "xray_native_runtime.h"

typedef struct XrtLocationOwner XrtLocationOwner;
typedef enum XrtLocationStatus {
    XRT_LOCATION_OK,
    XRT_LOCATION_INVALID,
    XRT_LOCATION_LIMIT,
    XRT_LOCATION_OUT_OF_MEMORY
} XrtLocationStatus;

// Output must be empty. Invalid calls preserve a nonempty output. Valid-call
// failure publishes nothing. Empty input is exactly (NULL, 0), with a real owner.
XR_FUNC XrtLocationStatus xrt_locations_create(const XrtSourceLocation *locations,
    size_t count, XrtLocationOwner **output);
// The caller already holds a live reference while retaining or reading. Atomic
// reference updates do not permit racing a borrowed view with its last release.
XR_FUNC bool xrt_locations_retain(XrtLocationOwner *owner);
XR_FUNC void xrt_locations_release(XrtLocationOwner *owner);
XR_FUNC size_t xrt_locations_count(const XrtLocationOwner *owner);
// ID0 is unknown. A nonzero ID above count is invalid; both return NULL and
// consumers must preserve that distinction. Returned paths are byte slices.
XR_FUNC const XrtSourceLocation *xrt_locations_get(const XrtLocationOwner *owner, uint32_t id);
#endif // XNATIVE_LOCATIONS_H
