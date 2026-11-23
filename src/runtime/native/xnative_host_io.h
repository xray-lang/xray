/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_io.h - Billed allocation and typed failure for host calls
 */
#ifndef XNATIVE_HOST_IO_H
#define XNATIVE_HOST_IO_H
#include "base/xio_policy.h"
#include "shared/xr_value_status.h"
#include "xray_native_managed.h"

XR_FUNC XrOsIoPolicy xrt_host_io_policy(XrtBillingOwner *);
XR_FUNC XrXirValueStatus xrt_host_io_failure(XrOsIoStatus);
#endif // XNATIVE_HOST_IO_H
