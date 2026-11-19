/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_set_backing.h - I64 membership and ordered replacement transactions
 */
#ifndef XNATIVE_SET_BACKING_H
#define XNATIVE_SET_BACKING_H
#include "xnative_backing_types.h"
#include "xnative_place.h"
#include "xray_native_set.h"
/* Callers admit the exact fixed Set plan before borrowing a backing or Place.
 * Shape checks below validate that admitted representation, not foreign types. */
XR_FUNC XrtManagedStatus xrt_set_backing_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const int64_t *, size_t count);
XR_FUNC XrtManagedStatus xrt_set_backing_contains(const XrtSetBacking *, int64_t, bool *found);
/* Select the live insertion ordinal without scanning all earlier holes.
 * Output changes only on success. This is not a new Source operation. */
XR_FUNC XrtManagedStatus xrt_set_backing_at(const XrtSetBacking *, size_t ordinal, int64_t *value);
/* The caller pins the backing through both formatter passes and publication. */
XR_FUNC XrtManagedStatus xrt_set_backing_display_held(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtSetBacking *);
/* The caller holds the real binding's mutation transaction. This allocation-
 * free attempt either commits fully or requests a candidate without editing.
 * Duplicate ADD, absent REMOVE and empty CLEAR do not detach a snapshot. */
XR_FUNC XrtManagedStatus xrt_set_backing_try_update(XrtSetBacking *,
    const XrtSetUpdate *, bool *applied, bool *removed);
XR_FUNC XrtManagedStatus xrt_set_backing_update_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtSetBacking *, const XrtSetUpdate *, bool *removed);
XR_FUNC XrtManagedStatus xrt_set_update_place(XrtPlace, XrtManagedFrame *, size_t workspace,
    const XrtBackingTypes *, const XrtSetUpdate *, bool *removed);
#endif // XNATIVE_SET_BACKING_H
