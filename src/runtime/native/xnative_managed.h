/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_managed.h - Internal support for the public managed storage ABI
 *
 * KEY CONCEPT:
 *   A physical object has one small header; immutable metadata owns its leases.
 */
#ifndef XNATIVE_MANAGED_H
#define XNATIVE_MANAGED_H
#include "base/xdefs.h"
#include "base/xsigned_rc.h"
#include "xray_native_managed.h"

/* The caller holds a real owner throughout this immutable metadata borrow.
 * Both callbacks identify the exact producer before its private context can be
 * interpreted. The result borrows the owner's Ops and grants no invocation or
 * mutation permission; no reference, allocation or provider callback occurs. */
XR_FUNC XrtManagedStatus xrt_managed_producer_borrow(const XrtManagedHeader *,
    bool (*retain)(void *), void (*drop)(void *), const XrtRuntimeTypeSpec **empty);
/* A prepared factory already owns its Ops before it has any live payload. */
XR_FUNC XrtManagedStatus xrt_managed_ops_producer_borrow(const XrtManagedOps *,
    bool (*retain)(void *), void (*drop)(void *), const XrtRuntimeTypeSpec **empty);
/* Compare immutable physical schemas of a real live owner. This neither proves
 * a producer identity nor grants a borrow or mutation permission. */
XR_FUNC bool xrt_managed_layout_matches(const XrtManagedHeader *, const XrtRuntimeTypeSpec *);

/* Trusted contiguous-tail producers publish their initialized row count after
 * installing each row, even on pending objects. Only this prefix owns child
 * fields. The remaining bytes are raw capacity and are never visited. */
XR_FUNC XrtManagedStatus xrt_runtime_type_plan_new_owned_prefix(XrtBillingOwner *,
    const XrtRuntimeTypeSpec *, size_t length_offset, XrtRuntimeTypePlan **);

/* Prepare a private replacement whose scalar tail is written before its
 * logical length is published. Only the fixed prefix is initialized here.
 * An owned field or a payload callback makes this preparation invalid. */
XR_FUNC XrtManagedStatus xrt_managed_prepare_scalar_tail(XrtManagedOps *, size_t bytes, XrtManagedPending *);
/* The caller holds the true local owner field and has ended every borrow of
 * the old allocation. Only a unique built-in scalar allocation with the
 * identical Ops can move. OK with an empty output leaves the binding intact.
 * Success lends the new allocation until the caller releases the binding. */
XR_FUNC XrtManagedStatus xrt_managed_grow_unique_scalar_tail(XrtManagedHeader **binding,
    XrtManagedOps *expected, size_t bytes, XrtManagedHeader **grown);
#endif // XNATIVE_MANAGED_H
