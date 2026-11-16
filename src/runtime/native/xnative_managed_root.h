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
typedef XrtManagedStatus (*XrtManagedReservedApply)(XrtManagedHeader *, void *);
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_begin(XrtManagedFrame *, size_t,
    XrtManagedSlotLocation binding, XrtManagedSlotReservation *empty);
/* Apply lends the current owner for an admitted allocation-free payload edit.
 * It never lends the owner field or grants mutation authority. The callback
 * cannot perform Root/Place operations, replace a binding, or change a domain.
 * Reentrant use of the registered handle is rejected during every callback. */
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_apply(XrtManagedSlotReservation *,
    XrtManagedReservedApply, void *);
XR_FUNC XrtManagedStatus xrt_managed_slot_reserved_prepare(const XrtManagedSlotReservation *,
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
