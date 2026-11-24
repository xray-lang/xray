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
#include "xray_native_string_value.h"
#include "xray_native_managed.h"

XR_FUNC XrOsIoPolicy xrt_host_io_policy(XrtBillingOwner *);
XR_FUNC XrXirValueStatus xrt_host_io_failure(XrOsIoStatus);
/* Private entry shared by the register VM and matching generated C. */
XRAY_API bool xrt_native_host_path_pair_bool(uint32_t callable, const XrtStringBacking *,
    const XrtStringBacking *, uint32_t source_id);
#endif // XNATIVE_HOST_IO_H
