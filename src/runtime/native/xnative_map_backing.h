/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_map_backing.h - Fixed String/I64 owners with independent insertion order
 *
 * KEY CONCEPT:
 *   Each capacity row has one real key owner or NULL. Bucket, insertion-order
 *   and rank fields are scalar indexes, never additional ownership edges.
 */
#ifndef XNATIVE_MAP_BACKING_H
#define XNATIVE_MAP_BACKING_H
#include "xnative_string_backing.h"

#include "xray_native_map.h"

typedef enum XrtMapUpdateKind {
    XRT_MAP_UPDATE_SET, XRT_MAP_UPDATE_REMOVE, XRT_MAP_UPDATE_CLEAR
} XrtMapUpdateKind;
typedef struct XrtMapUpdate {
    XrtMapUpdateKind operation;
    const XrtStringBacking *key;
    int64_t value;
} XrtMapUpdate;

/* Keys are admitted live String borrows. Equal bytes replace the value while
 * preserving the first stored key and its insertion position. Hashing uses the
 * existing deterministic FNV64 content seed, with no new per-instance seed. */
XR_FUNC XrtManagedStatus xrt_map_backing_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, XrtStringBacking *const *keys, const int64_t *, size_t count);
/* A miss publishes value=0 and found=false; a failure leaves both unchanged. */
XR_FUNC XrtManagedStatus xrt_map_backing_get(const XrtMapBacking *,
    const XrtStringBacking *, int64_t *value, bool *found);

/* The caller already owns mutation authority for the actual binding. A borrow
 * never grants it. No allocation occurs; OK with applied=false requires COW or
 * growth. Missing removal and empty clear complete without a write or detach.
 * Removed is published only when the operation actually completes. */
XR_FUNC XrtManagedStatus xrt_map_backing_try_update(XrtMapBacking *,
    const XrtMapUpdate *, bool *applied, bool *removed);
/* All candidate key owners are installed before one commit. Source and RHS
 * borrows remain live through that commit; failure preserves the old binding
 * and retires only this call's pending candidate. Call try_update first to avoid
 * allocating a candidate for an already complete no-op. */
XR_FUNC XrtManagedStatus xrt_map_backing_update_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtMapBacking *, const XrtMapUpdate *, bool *removed);
#endif // XNATIVE_MAP_BACKING_H
