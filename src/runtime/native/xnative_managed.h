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
#endif // XNATIVE_MANAGED_H
