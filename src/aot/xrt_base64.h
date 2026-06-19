/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_base64.h - Freestanding AOT Base64 helpers
 */

#ifndef XRT_BASE64_H
#define XRT_BASE64_H

#include "xrt_value.h"
#include "../shared/xr_base64_core.h"

static inline XrValue xrt_base64_is_valid(const char *data, int64_t len) {
    return XR_FROM_BOOL(xr_base64_core_is_valid(data, len < 0 ? 0 : (size_t) len));
}

#endif  // XRT_BASE64_H
