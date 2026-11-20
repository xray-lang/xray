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

/* This is the Frame's actual slot metadata, not a shadow owner directory.
 * owner_offset == SIZE_MAX uses committed; other offsets select the actual
 * Header field inside the same Frame payload. pending remains live state. */
typedef struct XrtManagedSlot {
    XrtManagedHeader *committed;
    XrtManagedPending pending;
    size_t owner_offset;
} XrtManagedSlot;

typedef struct XrtManagedFrameLocations {
    XrtManagedFrame *frame;
    void *payload;
    const XrtManagedSlot *slots;
    const bool *busy;
    size_t slot_count;
} XrtManagedFrameLocations;

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
/* Establish both empty output locations at the same lazy creation point.
 * Both remain unchanged on failure. The active frame owns the returned slab;
 * this cold boundary grants no additional access or borrowing authority. */
XRAY_API XrtManagedStatus xrt_managed_frame_enter_payload(XrtManagedRoot *,
    const XrtManagedFrameSpec *, XrtManagedFrame **empty_frame, void **empty_payload);
/* Establish one empty borrowed address view with the same real Frame factory.
 * Failure preserves every output field and success performs no extra allocation.
 * These addresses expire when the Frame leaves or its Root drains; active child
 * Frames do not move them. The view owns nothing and grants no access authority.
 * Only trusted execution code consuming a verified physical ownership plan on
 * the single executor may first read the actual busy flag and, when idle,
 * probe pending/owner fields in a contiguous window
 * with no call, retain, drop, allocation or other observation. Do not cache live
 * busy or pending values. All other Place use retains its original locking
 * protocol. A zero-slot Frame has slots == NULL; zero payload has payload == NULL. */
XRAY_API XrtManagedStatus xrt_managed_frame_enter_locations(XrtManagedRoot *,
    const XrtManagedFrameSpec *, XrtManagedFrameLocations *empty);
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
/* For a proved last use, stage one private, sole owner in an empty slot of the
 * same Frame. False leaves both slots unchanged for the ordinary retain. */
XRAY_API bool xrt_managed_slot_try_stage_unique(XrtManagedFrame *, size_t destination, size_t source);
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
 * borrows an active frame, acquires the returned domain before inspecting
 * pending/owner state, and releases it only after retirement callbacks finish.
 * A location selects exactly one bool busy or actual Cell Header domain.
 * These addresses never grant guest borrowing or mutation permissions. */
typedef struct XrtManagedSlotLocation {
    XrtManagedHeader **owner;
    const XrtManagedPending *pending;
    bool *busy;
    XrtManagedHeader *cell_domain;
} XrtManagedSlotLocation;
XRAY_API XrtManagedStatus xrt_managed_slot_location(XrtManagedFrame *, size_t,
    XrtManagedSlotLocation *);
#ifndef __cplusplus
typedef struct XrtManagedFrameForward {
    size_t source_slot, source_offset, destination_slot, destination_offset;
} XrtManagedFrameForward;

/* Trusted execution code consuming a verified physical ownership plan may
 * forward one owned Header through this activation's real factory view in a
 * single-executor window without calls, external observations or retirement.
 * This does not authenticate descriptors,
 * establish unique ownership or grant access to a binding. The caller proves
 * the adjacent logical COPY/DROP and preserves their ordinary fallback.
 * With an empty destination and 0 < RC < INT32_MAX, the original COPY retain
 * must succeed and its following source DROP cannot be last. Forwarding has
 * that net effect even with a real snapshot (RC == 2), whose reference stays
 * intact. No count, pending state or other bytes change. False leaves every
 * owner untouched; callers run the original COPY/DROP with fresh reads rather
 * than reusing a preflight pointer. C++ keeps Headers opaque. */
static inline bool xrt_managed_frame_try_forward(XrtManagedFrame *frame, void *payload,
    const XrtManagedFrameLocations *locations, XrtManagedFrameForward transfer) {
    size_t source=transfer.source_slot, source_offset=transfer.source_offset;
    size_t destination=transfer.destination_slot, destination_offset=transfer.destination_offset;
    if (!frame || !payload || !locations || locations->frame!=frame || locations->payload!=payload ||
        !locations->slots || !locations->busy || source>=locations->slot_count ||
        destination>=locations->slot_count || source==destination || source_offset==SIZE_MAX ||
        destination_offset==SIZE_MAX || source_offset==destination_offset ||
        locations->slots[source].owner_offset!=source_offset ||
        locations->slots[destination].owner_offset!=destination_offset) return false;
    if (*locations->busy) return false;
    if (locations->slots[source].pending.value || locations->slots[source].pending.allocation_bytes ||
        locations->slots[destination].pending.value || locations->slots[destination].pending.allocation_bytes)
        return false;
    XrtManagedHeader **from=(XrtManagedHeader **)((unsigned char *)payload+source_offset);
    XrtManagedHeader **to=(XrtManagedHeader **)((unsigned char *)payload+destination_offset);
    XrtManagedHeader *incoming=*from;
    if (*to || !incoming) return false;
    int32_t references=atomic_load_explicit(&incoming->references,memory_order_relaxed);
    if (references<=0 || references==INT32_MAX) return false;
    *to=incoming;
    *from=NULL;
    return true;
}
#endif
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_MANAGED_ROOT_H
