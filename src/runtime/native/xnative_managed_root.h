/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_managed_root.h - Internal support for the public stable-owner ABI
 *
 * KEY CONCEPT:
 *   Slots own real references and unpublished allocation tokens, never guest counts.
 */
#ifndef XNATIVE_MANAGED_ROOT_H
#define XNATIVE_MANAGED_ROOT_H
#include "xnative_managed.h"
#include "xray_native_managed_root.h"
/* A short native transaction, not a language borrowing permission. The actual
 * Root registers this handle's address and both stable fields. Copying the
 * handle does not copy that registration. Every callback returns normally. */
typedef struct XrtManagedSlotReservation { XrtManagedRoot *root; } XrtManagedSlotReservation;
/* One factory fills this slot's actual pending token. Finish retires its input
 * pins after commit/abort while the same Root still rejects reentry. Neither
 * callback may leave nonlocally or use Root/Place operations. Finish runs once
 * if the slot domain was entered, including a pending-conflict rejection. */
typedef struct XrtManagedSlotConstruction {
    XrtManagedStatus (*initialize)(XrtManagedHeader *, void *);
    void (*finish)(void *);
    void *context;
} XrtManagedSlotConstruction;
XR_FUNC XrtManagedStatus xrt_managed_slot_construct(XrtManagedFrame *, size_t,
    XrtManagedOps *, size_t bytes, const XrtManagedSlotConstruction *);
/* Borrow the actual Root of a live frame for a nested ordinary temporary
 * frame. This neither extends its lifetime nor authorizes a guest operation. */
XR_FUNC XrtManagedRoot *xrt_managed_frame_root(XrtManagedFrame *);

/* A Cell domain pins its real owner before entering its atomic Header guard.
 * A bool domain belongs to one executor. The descriptor stays borrowed and
 * unchanged until leave; entering confers no guest mutation permission. */
XR_FUNC bool xrt_managed_domain_valid(XrtManagedSlotLocation);
XR_FUNC bool xrt_managed_domain_same(XrtManagedSlotLocation, XrtManagedSlotLocation);
XR_FUNC XrtManagedStatus xrt_managed_domain_enter(XrtManagedSlotLocation);
XR_FUNC bool xrt_managed_domain_held(XrtManagedSlotLocation);
XR_FUNC void xrt_managed_domain_leave(XrtManagedSlotLocation);
typedef XrtManagedStatus (*XrtManagedReservedApply)(XrtManagedHeader *, void *);
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_begin(XrtManagedFrame *, size_t,
    XrtManagedSlotLocation binding, XrtManagedSlotReservation *empty);
/* Apply lends the current owner for an admitted allocation-free payload edit.
 * It never lends the owner field or grants mutation authority. The callback
 * cannot perform Root/Place operations, replace a binding, or change a domain.
 * Reentrant use of the registered handle is rejected during every callback. */
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_apply(XrtManagedSlotReservation *,
    XrtManagedReservedApply, void *);
/* Grow only the registered local binding during an empty reservation. The
 * caller has ended old payload borrows and must initialize newly used elements
 * before ending the reservation. An empty output leaves the old owner intact. */
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_grow_scalar_tail(const XrtManagedSlotReservation *,
    XrtManagedOps *expected, size_t bytes, XrtManagedHeader **grown);
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_prepare(const XrtManagedSlotReservation *,
    XrtManagedOps *, size_t, XrtManagedHeader **);
/* As above, with a scalar tail the factory initializes before publication. */
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_prepare_scalar_tail(const XrtManagedSlotReservation *,
    XrtManagedOps *, size_t, XrtManagedHeader **);
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_commit(const XrtManagedSlotReservation *);
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_abort(const XrtManagedSlotReservation *);
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_publish(XrtManagedSlotReservation *);
/* Result belongs to the registered candidate's Frame, differs from the source
 * and candidate, and has no pending token. Check it before any payload edit.
 * Consume publishes either the edited source (EMPTY phase) or the completed
 * candidate, clears source/candidate, then retires both replaced owners while
 * the original busy domain and handle registration remain held. */
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_result_check(const XrtManagedSlotReservation *, size_t result);
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_consume_publish(XrtManagedSlotReservation *, size_t result);
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_end(XrtManagedSlotReservation *);
#endif // XNATIVE_MANAGED_ROOT_H
