/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_managed_root.h - Stable private owners across nonlocal execution exits
 *
 * KEY CONCEPT:
 *   Slots own real references and unpublished allocation tokens, never guest counts.
 */
#ifndef XNATIVE_MANAGED_ROOT_H
#define XNATIVE_MANAGED_ROOT_H
#include "xnative_managed.h"

typedef struct XrtManagedRoot XrtManagedRoot;
typedef struct XrtManagedFrame XrtManagedFrame;

/* These private handles come only from their constructors. A Root is exclusively
 * owned outside the function containing setjmp. Frames and borrowed values are
 * valid only within their real active lifetimes; a retired handle is not an
 * input that this API can validate. No managed frame is needed for scalar calls.
 * One executor mutates a Root; concurrent use is unsupported and reentrant
 * mutation returns ARGUMENT. All cleanup callbacks must return normally. */
XR_FUNC XrtManagedStatus xrt_managed_root_new(XrtBillingOwner *, XrtManagedRoot **);
XR_FUNC XrtManagedStatus xrt_managed_root_drain(XrtManagedRoot *);
XR_FUNC XrtManagedStatus xrt_managed_root_destroy(XrtManagedRoot **);
XR_FUNC XrtManagedStatus xrt_managed_frame_enter(XrtManagedRoot *, size_t slots,
    XrtManagedFrame **);
/* Only the active top frame may leave. Its borrowed handle expires on success.
 * Leaving and draining abort partial values before dropping committed owners. */
XR_FUNC XrtManagedStatus xrt_managed_frame_leave(XrtManagedRoot *, XrtManagedFrame *);

/* Indices identify storage, not aliasing, borrow or coalescing permissions. A
 * Frame never moves or grows. Distinct slots holding the same pointer require
 * distinct real references. Adopt and move require an entirely empty target;
 * move is within one Root and clears its source. Only owned inputs may be
 * adopted. Borrowed outputs are not additional owners. */
XR_FUNC XrtManagedStatus xrt_managed_slot_adopt(XrtManagedFrame *, size_t,
    XrtManagedHeader **owned);
/* Publish an independently owned reference before dropping the old committed
 * owner. No allocation occurs. A pending value rejects the replacement and
 * preserves both owners. Equal pointers still represent two real references. */
XR_FUNC XrtManagedStatus xrt_managed_slot_replace(XrtManagedFrame *, size_t,
    XrtManagedHeader **owned);
/* Move one real committed reference to an empty external owner. The value may
 * have other owners. No allocation or reference-count change occurs; a pending
 * replacement rejects the transfer without changing either owner. */
XR_FUNC XrtManagedStatus xrt_managed_slot_take(XrtManagedFrame *, size_t,
    XrtManagedHeader **empty_owned);
XR_FUNC XrtManagedStatus xrt_managed_slot_borrow(XrtManagedFrame *, size_t,
    XrtManagedHeader **borrowed);
XR_FUNC XrtManagedStatus xrt_managed_slot_move(XrtManagedFrame *destination, size_t,
    XrtManagedFrame *source, size_t);
XR_FUNC XrtManagedStatus xrt_managed_slot_drop(XrtManagedFrame *, size_t);

/* Prepare installs the token in stable storage before exposing its payload.
 * The old committed reference remains owned until a successful commit publishes
 * its replacement. A rejected commit preserves both owners. Abort uses the
 * token's real extent and does not change the old committed value. */
XR_FUNC XrtManagedStatus xrt_managed_slot_prepare(XrtManagedFrame *, size_t,
    XrtManagedOps *, size_t bytes, XrtManagedHeader **borrowed_payload);
XR_FUNC XrtManagedStatus xrt_managed_slot_commit(XrtManagedFrame *, size_t);
XR_FUNC XrtManagedStatus xrt_managed_slot_abort(XrtManagedFrame *, size_t);

/* Internal location resolution for a bounded ownership transaction. The caller
 * borrows an active frame, acquires the returned busy domain before inspecting
 * pending/owner state, and releases it only after retirement callbacks finish.
 * These addresses never grant guest borrowing or mutation permissions. */
typedef struct XrtManagedSlotLocation {
    XrtManagedHeader **owner;
    const XrtManagedPending *pending;
    bool *busy;
} XrtManagedSlotLocation;
XR_FUNC XrtManagedStatus xrt_managed_slot_location(XrtManagedFrame *, size_t,
    XrtManagedSlotLocation *);
#endif // XNATIVE_MANAGED_ROOT_H
