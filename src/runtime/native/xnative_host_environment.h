/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_environment.h - Owned single-name environment lookup
 */
#ifndef XNATIVE_HOST_ENVIRONMENT_H
#define XNATIVE_HOST_ENVIRONMENT_H
#include "base/xio_policy.h"
#include "xnative_backing_types.h"

typedef struct XrtHostEnvironmentGet {
    XrtManagedStatus managed;
    XrOsIoStatus host;
    bool present;
} XrtHostEnvironmentGet;

/* A found value replaces the staging slot with an owned String, including for
 * empty values. Absence and failures preserve the slot. */
XR_FUNC XrtHostEnvironmentGet xrt_host_environment_get(XrtManagedFrame *, size_t slot,
    XrtBillingOwner *, const XrtBackingTypes *, const XrtManagedHeader *name);
#endif // XNATIVE_HOST_ENVIRONMENT_H
