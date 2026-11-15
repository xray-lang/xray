/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_managed.h - Private typed storage and independent billing lifetimes
 *
 * KEY CONCEPT:
 *   A physical object has one small header; immutable metadata owns its leases.
 */
#ifndef XNATIVE_MANAGED_H
#define XNATIVE_MANAGED_H
#include "base/xdefs.h"
#include "base/xsigned_rc.h"
#include <stddef.h>

typedef struct XrtBillingOwner XrtBillingOwner;
typedef struct XrtRuntimeTypePlan XrtRuntimeTypePlan;
typedef struct XrtManagedOps XrtManagedOps;
typedef struct XrtManagedHeader {
    _Atomic(int32_t) references;
    /* The high bit is reserved for dead-only retirement continuation state. */
    uint32_t flags;
    const XrtManagedOps *ops;
} XrtManagedHeader;
_Static_assert(sizeof(XrtManagedHeader) == 16, "managed storage has a sixteen-byte header");
_Static_assert(offsetof(XrtManagedHeader, references) == 0 &&
    offsetof(XrtManagedHeader, flags) == 4 && offsetof(XrtManagedHeader, ops) == 8,
    "managed header fields have fixed offsets");

typedef enum XrtManagedStatus {
    XRT_MANAGED_OK, XRT_MANAGED_ARGUMENT, XRT_MANAGED_OOM,
    XRT_MANAGED_LIMIT, XRT_MANAGED_CLOSED, XRT_MANAGED_REFCOUNT,
    XRT_MANAGED_BAD_UTF8, XRT_MANAGED_OVERFLOW
} XrtManagedStatus;

/* Every callback in this interface must return normally, without longjmp,
 * exceptions or another nonlocal exit. Retain may report failure only by false;
 * drop/deallocate/payload destruction cannot report failure. */
typedef struct XrtManagedLease {
    void *context;
    bool (*retain)(void *);
    void (*drop)(void *);
} XrtManagedLease;

/* NULL selects the static xr_malloc/xr_free provider without a synthetic lease.
 * A supplied provider owns its context/code through lease and supports concurrent
 * calls to every callback. Callbacks run without an index lock. Allocate reports
 * failure only by returning NULL; it must not leave its reservation through a nonlocal exit.
 * Successful storage has malloc/max_align_t alignment and remains valid until
 * the matching deallocate call. */
typedef struct XrtManagedAllocator {
    void *context;
    void *(*allocate)(void *, size_t);
    void (*deallocate)(void *, void *, size_t);
    XrtManagedLease lease;
} XrtManagedAllocator;
typedef struct XrtBillingStats {
    size_t bytes, blocks, allocations, fail_after;
    bool closed;
} XrtBillingStats;

/* No existing family or executor consumes this kernel. Handles are real owned
 * references, not validation of arbitrary pointers. Stats are exact at quiescence.
 * Closing prevents new reservations; already prepared allocations remain owned. */
XR_FUNC XrtManagedStatus xrt_billing_owner_new(const XrtManagedAllocator *, size_t limit,
    XrtBillingOwner **);
XR_FUNC bool xrt_billing_owner_retain(XrtBillingOwner *);
XR_FUNC void xrt_billing_owner_drop(XrtBillingOwner *);
XR_FUNC void xrt_billing_owner_close(XrtBillingOwner *);
XR_FUNC void xrt_billing_owner_fail_after(XrtBillingOwner *, size_t);
XR_FUNC XrtBillingStats xrt_billing_owner_stats(const XrtBillingOwner *);

/* This leaf physical schema contains sorted, nonoverlapping owned child pointer
 * fields in its fixed prefix and in each capacity row after fixed_size. A row
 * uses stride bytes; its owned offsets are relative to that row. Every allocated
 * owned slot holds a real reference or NULL, including holes and unused rows.
 * Length and initialization progress never bound ownership traversal. This does
 * not express a recursive graph of type plans. A zero
 * stride selects fixed_size; otherwise extent is fixed_size + capacity*stride,
 * with an aligned size_t capacity at capacity_offset within the fixed payload.
 * drop_payload clears additional payload only: it may not fail, longjmp, free the
 * header or release its last billing owner. It must tolerate zero initialization
 * and partial construction. Ordinary static runtime callbacks need no code lease;
 * callbacks/context that can retire must supply their actual owning lease. */
typedef struct XrtRuntimeTypeSpec {
    size_t fixed_size, alignment, capacity_offset, stride;
    const size_t *child_offsets;
    size_t child_count;
    void (*drop_payload)(XrtManagedHeader *, void *);
    XrtManagedLease code;
    const size_t *tail_child_offsets;
    size_t tail_child_count;
} XrtRuntimeTypeSpec;
XR_FUNC XrtManagedStatus xrt_runtime_type_plan_new(XrtBillingOwner *,
    const XrtRuntimeTypeSpec *, XrtRuntimeTypePlan **);
XR_FUNC bool xrt_runtime_type_plan_retain(XrtRuntimeTypePlan *);
XR_FUNC void xrt_runtime_type_plan_drop(XrtRuntimeTypePlan *);

/* The owner indexes Ops weakly. Lookup/retain and last-release/unlink share its
 * lock. Binding prepares outside that lock and refunds a losing candidate in
 * full. Each live owner/type pair has one stable Ops, never one per object. */
XR_FUNC XrtManagedStatus xrt_managed_ops_bind(XrtBillingOwner *, XrtRuntimeTypePlan *, XrtManagedOps **);
XR_FUNC bool xrt_managed_ops_retain(XrtManagedOps *);
XR_FUNC void xrt_managed_ops_drop(XrtManagedOps *);

/* A pending token is a move-only owner, not persistent object metadata. Prepare
 * zeroes the payload, installs true NULL in every declared fixed/tail slot and
 * sets the checked capacity from its actual allocation.
 * A caller may move real child owners into the declared zero-initialized fields.
 * Commit checks physical extent before publishing. Failure preserves both output
 * and pending; abort uses the token's real extent even after a rejected payload.
 * Retirement restores the physical capacity before the payload callback, which
 * observes cleared declared child slots even after a rejected capacity.
 * Root/unwind integration is deliberately absent: callers must close this token. */
typedef struct XrtManagedPending {
    XrtManagedHeader *value;
    size_t allocation_bytes;
} XrtManagedPending;
XR_FUNC XrtManagedStatus xrt_managed_prepare(XrtManagedOps *, size_t bytes, XrtManagedPending *);
XR_FUNC XrtManagedStatus xrt_managed_commit(XrtManagedPending *, XrtManagedHeader **);
XR_FUNC void xrt_managed_abort(XrtManagedPending *);
XR_FUNC bool xrt_managed_retain(XrtManagedHeader *);
/* Declared owned edges retire iteratively in child-first order. Only LAST
 * objects reuse their child storage, restored before their payload callback.
 * Callbacks may drop independently owned values, but may not inspect or retain
 * dead ancestors. Arbitrary callback recursion is not made stack-independent. */
XR_FUNC void xrt_managed_drop(XrtManagedHeader *);
#endif // XNATIVE_MANAGED_H
