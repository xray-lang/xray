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
#include "xnative_place.h"

#include "xray_native_map.h"

/* Keys are admitted live String borrows. Equal bytes replace the value while
 * preserving the first stored key and its insertion position. Hashing uses the
 * existing deterministic FNV64 content seed, with no new per-instance seed. */
XR_FUNC XrtManagedStatus xrt_map_backing_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, XrtStringBacking *const *keys, const int64_t *, size_t count);
/* Explicit entries remain distinct from overwriting spreads. Duplicate output
 * changes only on DUPLICATE_KEY and names the first and second explicit input
 * ordinals. All failures preserve the old slot and retire this pending value. */
XR_FUNC XrtManagedStatus xrt_map_backing_literal_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtMapLiteral *, const XrtMapLiteralInput *, XrtMapLiteralDuplicate *);
/* Synchronous constructor pins precede every allocator callback. Retain either
 * holds all inputs or refunds its prefix; release consumes that complete hold.
 * The caller still supplies real typed borrows, not arbitrary pointer bytes. */
XR_FUNC XrtManagedStatus xrt_map_literal_inputs_retain(const XrtMapLiteral *,
    const XrtMapLiteralInput *, size_t *capacity);
XR_FUNC void xrt_map_literal_inputs_drop(const XrtMapLiteral *, const XrtMapLiteralInput *);
/* A miss publishes value=0 and found=false; a failure leaves both unchanged. */
XR_FUNC XrtManagedStatus xrt_map_backing_get(const XrtMapBacking *,
    const XrtStringBacking *, int64_t *value, bool *found);
/* The caller holds an independent Map pin before preparing backing types and
 * until the Array commits or aborts. Keys retain their own String references. */
XR_FUNC XrtManagedStatus xrt_map_backing_snapshot_held(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtMapBacking *, bool keys);
XR_FUNC XrtManagedStatus xrt_map_backing_display_held(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtMapBacking *);

/* The caller already owns mutation authority for the actual binding. A borrow
 * never grants it. No allocation occurs; OK with applied=false requires COW or
 * growth. Missing removal and empty clear complete without a write or detach.
 * Removed is published only when the operation actually completes. */
XR_FUNC XrtManagedStatus xrt_map_backing_try_update(XrtMapBacking *,
    const XrtMapUpdate *, bool *applied, bool *removed);
/* The caller supplies an admitted local Map binding and its distinct update
 * workspace from the real private Frame. A miss leaves both slots and the Map unchanged. */
XR_FUNC bool xrt_map_try_overwrite_local(const XrtManagedFrameLocations *,
    size_t binding, size_t workspace, const XrtStringBacking *, int64_t);
/* All candidate key owners are installed before one commit. Source and RHS
 * borrows remain live through that commit; failure preserves the old binding
 * and retires only this call's pending candidate. Call try_update first to avoid
 * allocating a candidate for an already complete no-op. */
XR_FUNC XrtManagedStatus xrt_map_backing_update_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtMapBacking *, const XrtMapUpdate *, bool *removed);
/* The caller already owns mutation authority for this exact String/I64 Map
 * binding. REPLACE and a distinct empty workspace are reserved together; no
 * extra source retain forces COW. Publication precedes old-owner retirement,
 * and failure preserves the binding and removed while emptying this workspace.
 * Both storage domains reject reentry through allocation and retirement. */
XR_FUNC XrtManagedStatus xrt_map_update_place(XrtPlace, XrtManagedFrame *, size_t workspace,
    const XrtBackingTypes *, const XrtMapUpdate *, bool *removed);
#endif // XNATIVE_MAP_BACKING_H
