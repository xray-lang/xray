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
