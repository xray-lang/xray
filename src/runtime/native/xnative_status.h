/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_status.h - One boundary translation for managed storage failures
 *
 * KEY CONCEPT:
 *   Storage and diagnostic enums are different domains, never integer aliases.
 */
#ifndef XNATIVE_STATUS_H
#define XNATIVE_STATUS_H
#include "xnative_managed.h"
#include "shared/xr_value_status.h"

XR_FUNC XrXirValueStatus xrt_managed_value_status(XrtManagedStatus);
#endif // XNATIVE_STATUS_H
