/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_managed_root.h - Stable owners across nonlocal execution exits
 *
 * KEY CONCEPT:
 *   Slots own real references and unpublished allocation tokens, never guest counts.
 */
#ifndef XRAY_NATIVE_MANAGED_ROOT_H
#define XRAY_NATIVE_MANAGED_ROOT_H
#include "xray_native_managed.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XrtManagedRoot XrtManagedRoot;
typedef struct XrtManagedFrame XrtManagedFrame;

/* These handles come only from their constructors. A Root is exclusively
 * owned outside the function containing setjmp. Frames and borrowed values are
 * valid only within their real active lifetimes; a retired handle is not an
 * input that this API can validate. No managed frame is needed for scalar calls.
 * One executor mutates a Root; concurrent use is unsupported and reentrant
 * mutation returns ARGUMENT. All cleanup callbacks must return normally. */
XRAY_API XrtManagedStatus xrt_managed_root_new(XrtBillingOwner *, XrtManagedRoot **);
XRAY_API XrtManagedStatus xrt_managed_root_drain(XrtManagedRoot *);
XRAY_API XrtManagedStatus xrt_managed_root_destroy(XrtManagedRoot **);
XRAY_API XrtManagedStatus xrt_managed_frame_enter(XrtManagedRoot *, size_t slots,
    XrtManagedFrame **);
/* A function may keep inline aggregate homes in its one stable frame allocation.
 * SIZE_MAX selects the slot's ordinary Header* field. Other offsets select real
 * XrtManagedHeader* fields inside payload; they are strictly increasing among
 * bound slots, aligned and nonoverlapping. Typed family pointers are borrowed
 * casts of these Header* homes, never aliased through Header** lvalues.
 * The constructor copies all offsets and initializes every owner as true NULL.
 * The spec and vector are only borrowed until this call returns. Scalar-only
 * calls need no frame; this interface does not allocate a box per aggregate. */
typedef struct XrtManagedFrameSpec {
    size_t slot_count, payload_size, payload_alignment;
    const size_t *owned_offsets;
} XrtManagedFrameSpec;
XRAY_API XrtManagedStatus xrt_managed_frame_enter_layout(XrtManagedRoot *,
    const XrtManagedFrameSpec *, XrtManagedFrame **);
/* The borrowed slab lives exactly as long as the active frame. Scalar bytes may
 * be accessed by the admitted typed code. Its owner fields are the actual slot
 * storage, not a second set of references: copies, transfers and publication
 * still follow the verified ownership operations, including pending exclusion.
 * Obtaining an address grants no mutation or guest borrowing authority. */
XRAY_API XrtManagedStatus xrt_managed_frame_payload(XrtManagedFrame *, void **empty,
    size_t *bytes);
/* Only the active top frame may leave. Its borrowed handle expires on success.
 * Leaving and draining abort partial values before dropping committed owners. */
XRAY_API XrtManagedStatus xrt_managed_frame_leave(XrtManagedRoot *, XrtManagedFrame *);

/* Indices identify storage, not aliasing, borrow or coalescing permissions. A
 * Frame never moves or grows. Distinct slots holding the same pointer require
 * distinct real references. Adopt and move require an entirely empty target;
 * move is within one Root and clears its source. Only owned inputs may be
 * adopted. Borrowed outputs are not additional owners. */
XRAY_API XrtManagedStatus xrt_managed_slot_adopt(XrtManagedFrame *, size_t,
    XrtManagedHeader **owned);
/* Publish an independently owned reference before dropping the old committed
 * owner. No allocation occurs. A pending value rejects the replacement and
 * preserves both owners. Equal pointers still represent two real references. */
XRAY_API XrtManagedStatus xrt_managed_slot_replace(XrtManagedFrame *, size_t,
    XrtManagedHeader **owned);
/* Move one real committed reference to an empty external owner. The value may
 * have other owners. No allocation or reference-count change occurs; a pending
 * replacement rejects the transfer without changing either owner. */
XRAY_API XrtManagedStatus xrt_managed_slot_take(XrtManagedFrame *, size_t,
    XrtManagedHeader **empty_owned);
XRAY_API XrtManagedStatus xrt_managed_slot_borrow(XrtManagedFrame *, size_t,
    XrtManagedHeader **borrowed);
XRAY_API XrtManagedStatus xrt_managed_slot_move(XrtManagedFrame *destination, size_t,
    XrtManagedFrame *source, size_t);
XRAY_API XrtManagedStatus xrt_managed_slot_drop(XrtManagedFrame *, size_t);

/* Prepare installs the token in stable storage before exposing its payload.
 * The old committed reference remains owned until a successful commit publishes
 * its replacement. A rejected commit preserves both owners. Abort uses the
 * token's real extent and does not change the old committed value. */
XRAY_API XrtManagedStatus xrt_managed_slot_prepare(XrtManagedFrame *, size_t,
    XrtManagedOps *, size_t bytes, XrtManagedHeader **borrowed_payload);
XRAY_API XrtManagedStatus xrt_managed_slot_commit(XrtManagedFrame *, size_t);
XRAY_API XrtManagedStatus xrt_managed_slot_abort(XrtManagedFrame *, size_t);

/* Internal location resolution for a bounded ownership transaction. The caller
 * borrows an active frame, acquires the returned busy domain before inspecting
 * pending/owner state, and releases it only after retirement callbacks finish.
 * These addresses never grant guest borrowing or mutation permissions. */
typedef struct XrtManagedSlotLocation {
    XrtManagedHeader **owner;
    const XrtManagedPending *pending;
    bool *busy;
} XrtManagedSlotLocation;
XRAY_API XrtManagedStatus xrt_managed_slot_location(XrtManagedFrame *, size_t,
    XrtManagedSlotLocation *);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_MANAGED_ROOT_H
