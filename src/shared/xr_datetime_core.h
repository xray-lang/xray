/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_datetime_core.h - Pure DateTime helpers shared by VM stdlib and AOT
 */

#ifndef XR_DATETIME_CORE_H
#define XR_DATETIME_CORE_H

#include "../base/xplatform.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static inline int xr_datetime_core_is_leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static inline int xr_datetime_core_days_in_month(int year, int mon) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (mon < 1 || mon > 12)
        return 30;
    int d = days[mon - 1];
    if (mon == 2 && xr_datetime_core_is_leap_year(year))
        d = 29;
    return d;
}

/* Howard Hinnant's civil calendar algorithms, also used by C++20 chrono. */
static inline int64_t xr_datetime_core_days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned) (y - (int) (era * 400));
    const unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t) doe - 719468;
}

static inline void xr_datetime_core_civil_from_days(int64_t z, int *y_out, unsigned *m_out,
                                                    unsigned *d_out) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned) (z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int) ((int64_t) yoe + era * 400);
    const unsigned doy = doe - (365u * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5u * doy + 2) / 153;
    const unsigned d = doy - (153u * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    if (y_out)
        *y_out = y + (m <= 2 ? 1 : 0);
    if (m_out)
        *m_out = m;
    if (d_out)
        *d_out = d;
}

/* Treat the fields of `tm` as UTC and produce the corresponding time_t. */
static inline time_t xr_datetime_core_timegm(const struct tm *tm) {
    if (!tm)
        return (time_t) 0;
    int64_t days = xr_datetime_core_days_from_civil(tm->tm_year + 1900, (unsigned) (tm->tm_mon + 1),
                                                    (unsigned) tm->tm_mday);
    int64_t secs = days * 86400 + (int64_t) tm->tm_hour * 3600 + (int64_t) tm->tm_min * 60 +
                   (int64_t) tm->tm_sec;
    return (time_t) secs;
}

/* Decompose a UTC time_t into a struct tm. Valid for negative timestamps. */
static inline void xr_datetime_core_gmtime(time_t t, struct tm *tm) {
    if (!tm)
        return;
    int64_t s = (int64_t) t;
    int64_t days = s / 86400;
    int64_t secs_of_day = s % 86400;
    if (secs_of_day < 0) {
        secs_of_day += 86400;
        days -= 1;
    }

    int year;
    unsigned mon;
    unsigned mday;
    xr_datetime_core_civil_from_days(days, &year, &mon, &mday);

    tm->tm_year = year - 1900;
    tm->tm_mon = (int) mon - 1;
    tm->tm_mday = (int) mday;
    tm->tm_hour = (int) (secs_of_day / 3600);
    tm->tm_min = (int) ((secs_of_day % 3600) / 60);
    tm->tm_sec = (int) (secs_of_day % 60);

    int64_t w = (days + 4) % 7; /* 1970-01-01 was Thursday. */
    if (w < 0)
        w += 7;
    tm->tm_wday = (int) w;

    int64_t jan1 = xr_datetime_core_days_from_civil(year, 1, 1);
    tm->tm_yday = (int) (days - jan1);
    tm->tm_isdst = 0;
}

static inline int xr_datetime_core_local_offset_at(time_t t) {
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

static inline time_t xr_datetime_core_mktime(struct tm *tm, int is_utc) {
    if (!tm)
        return (time_t) 0;
    time_t wall_t = xr_datetime_core_timegm(tm);
    if (is_utc)
        return wall_t;
    int off_min = xr_datetime_core_local_offset_at(wall_t);
    return wall_t - (time_t) off_min * 60;
}

typedef struct XrDateTimeCoreFields {
    int64_t timestamp;
    int32_t milliseconds;
    int32_t tz_offset;
    int is_utc;
} XrDateTimeCoreFields;

typedef enum XrDateTimeCoreUnit {
    XR_DATETIME_CORE_UNIT_UNKNOWN = 0,
    XR_DATETIME_CORE_UNIT_MILLISECOND,
    XR_DATETIME_CORE_UNIT_SECOND,
    XR_DATETIME_CORE_UNIT_MINUTE,
    XR_DATETIME_CORE_UNIT_HOUR,
    XR_DATETIME_CORE_UNIT_DAY,
    XR_DATETIME_CORE_UNIT_WEEK,
    XR_DATETIME_CORE_UNIT_MONTH,
    XR_DATETIME_CORE_UNIT_YEAR,
} XrDateTimeCoreUnit;

static inline XrDateTimeCoreUnit xr_datetime_core_unit_from_cstr(const char *unit) {
    if (!unit)
        return XR_DATETIME_CORE_UNIT_UNKNOWN;
    if (strcmp(unit, "millisecond") == 0 || strcmp(unit, "milliseconds") == 0)
        return XR_DATETIME_CORE_UNIT_MILLISECOND;
    if (strcmp(unit, "second") == 0 || strcmp(unit, "seconds") == 0)
        return XR_DATETIME_CORE_UNIT_SECOND;
    if (strcmp(unit, "minute") == 0 || strcmp(unit, "minutes") == 0)
        return XR_DATETIME_CORE_UNIT_MINUTE;
    if (strcmp(unit, "hour") == 0 || strcmp(unit, "hours") == 0)
        return XR_DATETIME_CORE_UNIT_HOUR;
    if (strcmp(unit, "day") == 0 || strcmp(unit, "days") == 0)
        return XR_DATETIME_CORE_UNIT_DAY;
    if (strcmp(unit, "week") == 0 || strcmp(unit, "weeks") == 0)
        return XR_DATETIME_CORE_UNIT_WEEK;
    if (strcmp(unit, "month") == 0 || strcmp(unit, "months") == 0)
        return XR_DATETIME_CORE_UNIT_MONTH;
    if (strcmp(unit, "year") == 0 || strcmp(unit, "years") == 0)
        return XR_DATETIME_CORE_UNIT_YEAR;
    return XR_DATETIME_CORE_UNIT_UNKNOWN;
}

static inline void xr_datetime_core_to_tm_fields(const XrDateTimeCoreFields *dt, struct tm *tm) {
    if (!dt || !tm)
        return;
    time_t t = (time_t) dt->timestamp;
    if (!dt->is_utc)
        t += (time_t) dt->tz_offset * 60;
    xr_datetime_core_gmtime(t, tm);
}

static inline void xr_datetime_core_add_milliseconds(const XrDateTimeCoreFields *dt, int64_t amount,
                                                     XrDateTimeCoreFields *out) {
    int64_t total_ms = dt->timestamp * 1000 + dt->milliseconds + amount;
    *out = *dt;
    out->timestamp = total_ms / 1000;
    out->milliseconds = (int32_t) (total_ms % 1000);
    if (out->milliseconds < 0) {
        out->timestamp--;
        out->milliseconds += 1000;
    }
}

static inline int xr_datetime_core_add_fields(const XrDateTimeCoreFields *dt, int64_t amount,
                                              const char *unit, XrDateTimeCoreFields *out) {
    if (!dt || !out)
        return 0;
    XrDateTimeCoreUnit kind = xr_datetime_core_unit_from_cstr(unit);
    *out = *dt;

    switch (kind) {
        case XR_DATETIME_CORE_UNIT_MILLISECOND:
            xr_datetime_core_add_milliseconds(dt, amount, out);
            return 1;
        case XR_DATETIME_CORE_UNIT_SECOND:
            out->timestamp = dt->timestamp + amount;
            return 1;
        case XR_DATETIME_CORE_UNIT_MINUTE:
            out->timestamp = dt->timestamp + amount * 60;
            return 1;
        case XR_DATETIME_CORE_UNIT_HOUR:
            out->timestamp = dt->timestamp + amount * 3600;
            return 1;
        case XR_DATETIME_CORE_UNIT_DAY:
            out->timestamp = dt->timestamp + amount * 86400;
            return 1;
        case XR_DATETIME_CORE_UNIT_WEEK:
            out->timestamp = dt->timestamp + amount * 604800;
            return 1;
        case XR_DATETIME_CORE_UNIT_MONTH: {
            struct tm tm;
            xr_datetime_core_to_tm_fields(dt, &tm);
            int total_months = (tm.tm_year + 1900) * 12 + tm.tm_mon + (int) amount;
            tm.tm_year = total_months / 12 - 1900;
            tm.tm_mon = total_months % 12;
            if (tm.tm_mon < 0) {
                tm.tm_mon += 12;
                tm.tm_year--;
            }
            int max_day = xr_datetime_core_days_in_month(tm.tm_year + 1900, tm.tm_mon + 1);
            if (tm.tm_mday > max_day)
                tm.tm_mday = max_day;
            tm.tm_isdst = -1;
            out->timestamp = (int64_t) xr_datetime_core_mktime(&tm, dt->is_utc);
            return 1;
        }
        case XR_DATETIME_CORE_UNIT_YEAR: {
            struct tm tm;
            xr_datetime_core_to_tm_fields(dt, &tm);
            tm.tm_year += (int) amount;
            int max_day = xr_datetime_core_days_in_month(tm.tm_year + 1900, tm.tm_mon + 1);
            if (tm.tm_mday > max_day)
                tm.tm_mday = max_day;
            tm.tm_isdst = -1;
            out->timestamp = (int64_t) xr_datetime_core_mktime(&tm, dt->is_utc);
            return 1;
        }
        case XR_DATETIME_CORE_UNIT_UNKNOWN:
        default:
            out->timestamp = dt->timestamp + amount;
            return 0;
    }
}

static inline int64_t xr_datetime_core_diff_fields(const XrDateTimeCoreFields *a,
                                                   const XrDateTimeCoreFields *b,
                                                   const char *unit) {
    if (!a || !b)
        return 0;
    int64_t diff_ms = (a->timestamp - b->timestamp) * 1000 + (a->milliseconds - b->milliseconds);
    switch (xr_datetime_core_unit_from_cstr(unit)) {
        case XR_DATETIME_CORE_UNIT_MILLISECOND:
            return diff_ms;
        case XR_DATETIME_CORE_UNIT_MINUTE:
            return diff_ms / 60000;
        case XR_DATETIME_CORE_UNIT_HOUR:
            return diff_ms / 3600000;
        case XR_DATETIME_CORE_UNIT_DAY:
            return diff_ms / 86400000;
        case XR_DATETIME_CORE_UNIT_WEEK:
            return diff_ms / 604800000;
        case XR_DATETIME_CORE_UNIT_SECOND:
        case XR_DATETIME_CORE_UNIT_MONTH:
        case XR_DATETIME_CORE_UNIT_YEAR:
        case XR_DATETIME_CORE_UNIT_UNKNOWN:
        default:
            return diff_ms / 1000;
    }
}

typedef int (*XrDateTimeCoreWriteFn)(void *ctx, const char *data, size_t len);

typedef struct XrDateTimeCoreWriter {
    void *ctx;
    XrDateTimeCoreWriteFn write;
} XrDateTimeCoreWriter;

static inline int xr_datetime_core_write_bytes(XrDateTimeCoreWriter *writer, const char *data,
                                               size_t len) {
    if (!writer || !writer->write || (!data && len != 0))
        return -1;
    if (len == 0)
        return 0;
    return writer->write(writer->ctx, data, len);
}

static inline int64_t xr_datetime_core_write_i32(XrDateTimeCoreWriter *writer, const char *fmt,
                                                 int value) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), fmt, value);
    if (n < 0 || n >= (int) sizeof(tmp))
        return -1;
    if (xr_datetime_core_write_bytes(writer, tmp, (size_t) n) != 0)
        return -1;
    return n;
}

static inline int64_t xr_datetime_core_format_tm(XrDateTimeCoreWriter *writer, const struct tm *tm,
                                                 int milliseconds, const char *pattern,
                                                 int64_t pattern_len) {
    if (!writer || !tm || !pattern)
        return -1;
    if (pattern_len < 0)
        pattern_len = (int64_t) strlen(pattern);

    int64_t total = 0;
    for (int64_t i = 0; i < pattern_len;) {
        const char *p = pattern + i;
        int64_t left = pattern_len - i;
        int64_t wrote = -1;
        if (left >= 4 && strncmp(p, "YYYY", 4) == 0) {
            wrote = xr_datetime_core_write_i32(writer, "%04d", tm->tm_year + 1900);
            i += 4;
        } else if (left >= 2 && strncmp(p, "MM", 2) == 0 && (left == 2 || p[2] != 'M')) {
            wrote = xr_datetime_core_write_i32(writer, "%02d", tm->tm_mon + 1);
            i += 2;
        } else if (left >= 2 && strncmp(p, "DD", 2) == 0) {
            wrote = xr_datetime_core_write_i32(writer, "%02d", tm->tm_mday);
            i += 2;
        } else if (left >= 2 && strncmp(p, "HH", 2) == 0) {
            wrote = xr_datetime_core_write_i32(writer, "%02d", tm->tm_hour);
            i += 2;
        } else if (left >= 2 && strncmp(p, "mm", 2) == 0) {
            wrote = xr_datetime_core_write_i32(writer, "%02d", tm->tm_min);
            i += 2;
        } else if (left >= 2 && strncmp(p, "ss", 2) == 0) {
            wrote = xr_datetime_core_write_i32(writer, "%02d", tm->tm_sec);
            i += 2;
        } else if (left >= 3 && strncmp(p, "SSS", 3) == 0) {
            wrote = xr_datetime_core_write_i32(writer, "%03d", milliseconds);
            i += 3;
        } else {
            if (xr_datetime_core_write_bytes(writer, p, 1) != 0)
                return -1;
            wrote = 1;
            i += 1;
        }
        if (wrote < 0)
            return -1;
        total += wrote;
    }
    return total;
}

static inline int64_t xr_datetime_core_iso_write(XrDateTimeCoreWriter *writer, const struct tm *tm,
                                                 int milliseconds, int is_utc, int tz_offset) {
    if (!writer || !tm)
        return -1;
    char tmp[96];
    int n;
    if (is_utc) {
        n = snprintf(tmp, sizeof(tmp), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm->tm_year + 1900,
                     tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
                     milliseconds);
    } else {
        int off = tz_offset;
        char sign = off >= 0 ? '+' : '-';
        if (off < 0)
            off = -off;
        n = snprintf(tmp, sizeof(tmp), "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d:%02d",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min,
                     tm->tm_sec, milliseconds, sign, off / 60, off % 60);
    }
    if (n < 0 || n >= (int) sizeof(tmp))
        return -1;
    if (xr_datetime_core_write_bytes(writer, tmp, (size_t) n) != 0)
        return -1;
    return n;
}

#endif  // XR_DATETIME_CORE_H
