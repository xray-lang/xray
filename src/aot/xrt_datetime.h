/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_datetime.h - Freestanding AOT helpers for DateTime module queries.
 */

#ifndef XRT_DATETIME_H
#define XRT_DATETIME_H

#include "../base/xplatform.h"
#include "../shared/xr_datetime_core.h"
#include "xrt_value.h"
#include <time.h>

static inline XrValue xrt_datetime_offset(void) {
    return XR_FROM_INT((int64_t) xr_datetime_core_local_offset_at(time(NULL)));
}

#endif  // XRT_DATETIME_H
