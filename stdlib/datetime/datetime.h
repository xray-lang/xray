/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * datetime.h - DateTime standard library
 *
 * KEY CONCEPT:
 *   DateTime is a native type with method syntax (dt.year(), dt.format(), etc).
 *   Module-level functions are only for creation: now(), utc(), create(), parse(),
 *   fromTimestamp(), offset(). All other operations use dot syntax on DateTime objects.
 *
 * FORMAT PLACEHOLDERS:
 *   YYYY - 4-digit year       MM - month (01-12)     DD - day (01-31)
 *   HH   - hour (00-23)       mm - minute (00-59)    ss - second (00-59)
 *   SSS  - milliseconds (000-999)
 */

#ifndef XR_STDLIB_DATETIME_H
#define XR_STDLIB_DATETIME_H

#include "../../src/base/xdefs.h"
#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"
#include <time.h>

/* ========== DateTime Structure ========== */

typedef struct XrDateTime {
    XrGCHeader gc;
    int64_t timestamp;      // Unix timestamp (seconds)
    int32_t milliseconds;   // Millisecond part (0-999)
    int16_t tz_offset;      // Timezone offset (minutes)
    uint8_t is_utc;         // 1 if UTC, 0 if local time
    uint8_t _pad;           // Explicit padding
} XrDateTime;

/* ========== Creation API ========== */

XR_FUNC XrDateTime* xr_datetime_now(XrayIsolate *isolate);
XR_FUNC XrDateTime* xr_datetime_utc(XrayIsolate *isolate);
XR_FUNC XrDateTime* xr_datetime_create(XrayIsolate *isolate, int year, int month, int day,
                                       int hour, int minute, int second, int is_utc);
XR_FUNC XrDateTime* xr_datetime_from_timestamp(XrayIsolate *isolate, int64_t timestamp);
XR_FUNC XrDateTime* xr_datetime_from_timestamp_ms(XrayIsolate *isolate, int64_t timestamp_ms);
XR_FUNC XrDateTime* xr_datetime_parse(XrayIsolate *isolate, const char *str, const char *format);

/* ========== Format API ========== */

XR_FUNC int xr_datetime_format(XrDateTime *dt, const char *pattern, char *buf, size_t buf_size);
XR_FUNC int xr_datetime_to_iso_string(XrDateTime *dt, char *buf, size_t buf_size);

/* ========== Component Access API ========== */

XR_FUNC int xr_datetime_year(XrDateTime *dt);
XR_FUNC int xr_datetime_month(XrDateTime *dt);
XR_FUNC int xr_datetime_day(XrDateTime *dt);
XR_FUNC int xr_datetime_hour(XrDateTime *dt);
XR_FUNC int xr_datetime_minute(XrDateTime *dt);
XR_FUNC int xr_datetime_second(XrDateTime *dt);
XR_FUNC int xr_datetime_millisecond(XrDateTime *dt);
XR_FUNC int xr_datetime_weekday(XrDateTime *dt);
XR_FUNC int xr_datetime_yearday(XrDateTime *dt);

/* ========== Comparison API ========== */

XR_FUNC int xr_datetime_is_before(XrDateTime *dt1, XrDateTime *dt2);
XR_FUNC int xr_datetime_is_after(XrDateTime *dt1, XrDateTime *dt2);
XR_FUNC int xr_datetime_equals(XrDateTime *dt1, XrDateTime *dt2);

/* ========== Utility API ========== */

XR_FUNC int xr_datetime_is_leap_year(XrDateTime *dt);
XR_FUNC int xr_datetime_days_in_month(XrDateTime *dt);

/* ========== Date Arithmetic API ========== */

XR_FUNC XrDateTime* xr_datetime_add(XrayIsolate *isolate, XrDateTime *dt, int64_t amount, const char *unit);
XR_FUNC int64_t xr_datetime_diff(XrDateTime *dt1, XrDateTime *dt2, const char *unit);

/* ========== Timezone API ========== */

XR_FUNC XrDateTime* xr_datetime_to_utc(XrayIsolate *isolate, XrDateTime *dt);
XR_FUNC XrDateTime* xr_datetime_to_local(XrayIsolate *isolate, XrDateTime *dt);
XR_FUNC int xr_datetime_local_offset(void);

/* ========== Utility Functions ========== */

XR_FUNC void xr_datetime_to_tm(XrDateTime *dt, struct tm *tm);

/* ========== XrValue Conversion ========== */

static inline XrValue xr_datetime_value(XrDateTime *dt) {
    return XR_FROM_PTR(dt);
}

#define XR_IS_DATETIME(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TDATETIME)
#define XR_TO_DATETIME(v) ((XrDateTime*)XR_TO_PTR(v))

/* ========== Module Loading ========== */

XrModule* xr_load_module_datetime(XrayIsolate *isolate);

#endif
