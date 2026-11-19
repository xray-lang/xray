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
#include "base/xhash.h"
#include "shared/xr_value_format_core.h"

/* The typed factory initializes the cache before exposing its pending bytes. */
static inline XrtManagedStatus xrt_string_backing_prepare(XrtManagedFrame *frame, size_t slot,
    XrtManagedOps *ops, size_t extent, XrtStringBacking **output) {
    XrtManagedHeader *pending = NULL;
    XrtManagedStatus status = xrt_managed_slot_prepare(frame, slot, ops, extent, &pending);
    if (status == XRT_MANAGED_OK) {
        XrtStringBacking *text = (XrtStringBacking *)pending;
        atomic_init(&text->cached_hash, 0);
        *output = text;
    }
    return status;
}

/* A live borrow keeps the bytes stable. Readers may race to cache the same
 * derived hash; the atomic cache neither publishes bytes nor grants mutation.
 * Zero remains a valid hash and is simply recomputed on its next use. */
static inline uint64_t xrt_string_backing_hash(const XrtStringBacking *text) {
    uint64_t hash = atomic_load_explicit(&text->cached_hash, memory_order_relaxed);
    if (!hash) {
        hash = xr_hash_bytes64(text->bytes, text->length);
        atomic_store_explicit(&((XrtStringBacking *)text)->cached_hash, hash, memory_order_relaxed);
    }
    return hash;
}

/* Reserve counts tail bytes, including the terminator; zero selects length+1.
 * Inputs are synchronous borrows. The stable pending slot exists before any
 * payload write. Failure aborts only this factory's pending token and preserves
 * the old committed owner. Success leaves one complete owner in the slot. */
XR_FUNC XrtManagedStatus xrt_string_backing_create(XrtManagedFrame *, size_t slot,
    const XrtBackingTypes *, const char *bytes, size_t length, size_t reserve);
XR_FUNC XrtManagedStatus xrt_string_backing_concat_create(XrtManagedFrame *, size_t slot,
    const XrtBackingTypes *, const XrtStringBacking *, const XrtStringBacking *, size_t reserve);
/* The adapter must keep every formatted borrow stable through allocation. */
XR_FUNC XrtManagedStatus xrt_string_backing_format_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, XrValueFormatReader, XrValueFormatNode);
#endif // XNATIVE_STRING_BACKING_H
