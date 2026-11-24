/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_value_status.h - Shared failure facts independent of physical value storage
 *
 * KEY CONCEPT:
 *   Checked values and execution diagnostics use one established status domain.
 */
#ifndef XR_VALUE_STATUS_H
#define XR_VALUE_STATUS_H

typedef enum XrXirValueStatus {
    XR_XIR_VALUE_OK, XR_XIR_VALUE_BAD_ARGUMENT, XR_XIR_VALUE_BAD_UTF8,
    XR_XIR_VALUE_OOM, XR_XIR_VALUE_LIMIT, XR_XIR_VALUE_REFCOUNT_LIMIT,
    XR_XIR_VALUE_BOUNDS, XR_XIR_VALUE_UNSUPPORTED,
    XR_XIR_VALUE_MEMORY_LIMIT, XR_XIR_VALUE_OVERFLOW, XR_XIR_VALUE_IO,
    XR_XIR_VALUE_DUPLICATE_KEY, XR_XIR_VALUE_CANCELLED
} XrXirValueStatus;

#endif // XR_VALUE_STATUS_H
