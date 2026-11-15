/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_array_ops.h - Binding-neutral operations on admitted fixed Array storage
 *
 * KEY CONCEPT:
 *   Mutation authority belongs to the caller's live binding. A borrowed unique
 *   body can be updated without manufacturing an extra Root owner first.
 */
#ifndef XNATIVE_ARRAY_OPS_H
#define XNATIVE_ARRAY_OPS_H
#include "xnative_array_backing.h"
#include "xnative_place.h"

/* The caller has already admitted the exact Array kind and separately owns
 * mutation authority for its current binding. These borrows establish neither
 * permission nor a new owner. All source and RHS borrows survive until return.
 * OK with applied=false requests the allocating path and changes no payload.
 * A successful in-place String update retains before publishing and drops the
 * old child afterward. The function never allocates or changes the binding. */
XR_FUNC XrtManagedStatus xrt_array_backing_try_update(XrtArrayBacking *,
    const XrtArrayUpdate *, bool *applied);

/* Build the complete edited candidate before publishing once. The source may
 * be the destination's current committed body, another field or a snapshot.
 * Any failure retires only this call's pending body and preserves the binding;
 * discarded source elements are never copied or needlessly retained. */
XR_FUNC XrtManagedStatus xrt_array_backing_update_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtArrayBacking *, const XrtArrayUpdate *);
/* The receiving boundary has already admitted a real Array owner in binding.
 * binding_kind comes from that binding's exact payload descriptor,
 * independently of the RHS item. A mismatch rejects before payload access.
 * This is not validation of arbitrary headers or a grant of mutation rights.
 * The caller keeps types and all RHS borrows live. The candidate is a distinct
 * true-empty workspace in an active stable frame. No extra binding reference
 * is acquired; failure preserves the binding and empties this workspace.
 * Both in-place retirement and allocating replacement retain the actual
 * domains through callbacks. The caller may fail nonlocally only after return. */
XR_FUNC XrtManagedStatus xrt_array_update_place(XrtPlace, XrtBackingArrayKind binding_kind,
    XrtManagedFrame *, size_t temporary, const XrtBackingTypes *, const XrtArrayUpdate *);

/* Scalar results are copied. A String result is only a synchronous borrow of
 * its source tail owner; an independent binding must retain it explicitly.
 * Negative and past-end indices return BOUNDS without changing output. */
XR_FUNC XrtManagedStatus xrt_array_backing_get(const XrtArrayBacking *,
    XrtBackingArrayKind, int64_t index, XrtArrayItem *output);
XR_FUNC XrtManagedStatus xrt_array_backing_repeat_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtArrayItem *fill, int64_t length);
/* Display consumes the shared pure formatting rules through a physical layout
 * adapter and commits one complete String, without an intermediate value ABI. */
XR_FUNC XrtManagedStatus xrt_array_backing_display_create(XrtManagedFrame *, size_t,
    const XrtBackingTypes *, const XrtArrayBacking *, XrtBackingArrayKind);
#endif // XNATIVE_ARRAY_OPS_H
