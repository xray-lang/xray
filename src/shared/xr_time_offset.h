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
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* March-based Gregorian day numbering avoids applying the host timezone a
 * second time through mktime. The epoch cancels when the two days differ. */
static inline int64_t xr_time_calendar_day(const struct tm *value) {
    int64_t year = (int64_t) value->tm_year + 1900;
    int64_t month = (int64_t) value->tm_mon + 1;
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int64_t year_of_era = year - era * 400;
    int64_t march_month = month + (month > 2 ? -3 : 9);
    int64_t day_of_year = (153 * march_month + 2) / 5 + value->tm_mday - 1;
    return era * 146097 + year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
}

/* Failure publishes no result and never substitutes a different timestamp.
 * A sub-minute historical offset is truncated toward zero. */
static inline bool xr_time_utc_offset_at(int64_t seconds, int64_t *result_out) {
    if (!result_out)
        return false;
    _Static_assert((time_t) -1 < (time_t) 0, "UTC queries require signed time_t");
    time_t probe = (time_t) seconds;
    if ((int64_t) probe != seconds)
        return false;
    struct tm local_tm;
    struct tm utc_tm;
#ifdef XR_OS_WINDOWS
    if (localtime_s(&local_tm, &probe) != 0 || gmtime_s(&utc_tm, &probe) != 0)
        return false;
#else
    if (!localtime_r(&probe, &local_tm) || !gmtime_r(&probe, &utc_tm))
        return false;
#endif
    int64_t days = xr_time_calendar_day(&local_tm) - xr_time_calendar_day(&utc_tm);
    int64_t offset = days * 86400 + ((int64_t) local_tm.tm_hour - utc_tm.tm_hour) * 3600 +
                     ((int64_t) local_tm.tm_min - utc_tm.tm_min) * 60 + local_tm.tm_sec -
                     utc_tm.tm_sec;
    *result_out = offset / 60;
    return true;
}

#endif  // XR_TIME_OFFSET_H
