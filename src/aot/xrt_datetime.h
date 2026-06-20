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

#include "xrt_value.h"
#include "../base/xplatform.h"
#include <time.h>

static inline int xrt_datetime_local_offset_at(time_t t) {
    struct tm local_tm;
    struct tm utc_tm;
    time_t probe = t;
#ifdef XR_OS_WINDOWS
    if (probe < 86400)
        probe = 86400;
    localtime_s(&local_tm, &probe);
    gmtime_s(&utc_tm, &probe);
#else
    localtime_r(&probe, &local_tm);
    gmtime_r(&probe, &utc_tm);
#endif
    local_tm.tm_isdst = 0;
    utc_tm.tm_isdst = 0;
    return (int) (difftime(mktime(&local_tm), mktime(&utc_tm)) / 60.0);
}

static inline XrValue xrt_datetime_offset(void) {
    return XR_FROM_INT((int64_t) xrt_datetime_local_offset_at(time(NULL)));
}

#endif  // XRT_DATETIME_H
