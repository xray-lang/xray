/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_status.c - Preserve the actual reason for a managed value failure
 *
 * KEY CONCEPT:
 *   In particular allocation failure cannot become malformed UTF-8 by a cast.
 */
#include "xnative_status.h"

XR_FUNCDEF XrXirValueStatus xrt_managed_value_status(XrtManagedStatus status) {
    switch (status) {
    case XRT_MANAGED_OK: return XR_XIR_VALUE_OK;
    case XRT_MANAGED_ARGUMENT: case XRT_MANAGED_CLOSED: return XR_XIR_VALUE_BAD_ARGUMENT;
    case XRT_MANAGED_OOM: return XR_XIR_VALUE_OOM;
    case XRT_MANAGED_LIMIT: return XR_XIR_VALUE_MEMORY_LIMIT;
    case XRT_MANAGED_REFCOUNT: return XR_XIR_VALUE_REFCOUNT_LIMIT;
    case XRT_MANAGED_BAD_UTF8: return XR_XIR_VALUE_BAD_UTF8;
    case XRT_MANAGED_OVERFLOW: return XR_XIR_VALUE_OVERFLOW;
    case XRT_MANAGED_BOUNDS: return XR_XIR_VALUE_BOUNDS;
    default: return XR_XIR_VALUE_BAD_ARGUMENT;
    }
}
