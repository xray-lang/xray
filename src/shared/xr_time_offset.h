/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_time_offset.h - Local UTC offset helper for the native time boundary.
 */

#ifndef XR_TIME_OFFSET_H
#define XR_TIME_OFFSET_H

#include "../base/xplatform.h"
#include <time.h>

static inline int xr_time_utc_offset_at(time_t t) {
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

#endif  // XR_TIME_OFFSET_H
