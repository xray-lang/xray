/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * datetime.c - DateTime standard library implementation
 *
 * KEY CONCEPT:
 *   DateTime is a native type. Module exports only factory functions
 *   (now/utc/create/parse/fromTimestamp/offset). All component access,
 *   arithmetic, comparison, and timezone operations use dot syntax.
 */

#include "datetime.h"
#include "../common.h"
#include "../ctxbuf.h"
#include "../../include/xray_platform.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/gc/xgc.h"
#include "../../src/base/xchecks.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#define XR_INT(n) XR_FROM_INT(n)

#ifndef XR_PLATFORM_WINDOWS
#include <sys/time.h>
#endif

/* ========== Internal Helpers ========== */

static XrDateTime *datetime_alloc(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "datetime_alloc: isolate must not be NULL");
    XrDateTime *dt = (XrDateTime *) xr_gc_alloc(&isolate->gc, sizeof(XrDateTime), XR_TDATETIME);
    if (!dt)
        return NULL;
    dt->timestamp = 0;
    dt->milliseconds = 0;
    dt->tz_offset = 0;
    dt->is_utc = 0;
    dt->_pad = 0;
    return dt;
}

static int64_t get_current_millis(void) {
#ifdef XR_PLATFORM_WINDOWS
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int64_t t = ((int64_t) ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000LL) / 10000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static int days_in_month_table(int year, int mon) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    XR_DCHECK(year >= -9999 && year <= 9999, "days_in_month: year in sane range");
    if (mon < 1 || mon > 12)
        return 30;
    int d = days[mon - 1];
    if (mon == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
        d = 29;
    return d;
}

static inline time_t datetime_mktime(struct tm *tm, int is_utc) {
    XR_DCHECK(tm != NULL, "datetime_mktime: tm must not be NULL");
    if (is_utc) {
#ifdef _WIN32
        return _mkgmtime(tm);
#else
        return timegm(tm);
#endif
    }
    return mktime(tm);
}

/* ========== Timezone ========== */

int xr_datetime_local_offset(void) {
    time_t now = time(NULL);
    struct tm local_tm, utc_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &now);
    gmtime_s(&utc_tm, &now);
#else
    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);
#endif
    local_tm.tm_isdst = 0;
    utc_tm.tm_isdst = 0;
    double diff_sec = difftime(mktime(&local_tm), mktime(&utc_tm));
    return (int) (diff_sec / 60.0);
}

/* ========== Creation API ========== */

XrDateTime *xr_datetime_now(XrayIsolate *isolate) {
    XrDateTime *dt = datetime_alloc(isolate);
    int64_t millis = get_current_millis();
    dt->timestamp = millis / 1000;
    dt->milliseconds = millis % 1000;
    dt->tz_offset = xr_datetime_local_offset();
    dt->is_utc = 0;
    return dt;
}

XrDateTime *xr_datetime_utc(XrayIsolate *isolate) {
    XrDateTime *dt = datetime_alloc(isolate);
    int64_t millis = get_current_millis();
    dt->timestamp = millis / 1000;
    dt->milliseconds = millis % 1000;
    dt->tz_offset = 0;
    dt->is_utc = 1;
    return dt;
}

XrDateTime *xr_datetime_create(XrayIsolate *isolate, int year, int month, int day, int hour,
                               int minute, int second, int is_utc) {
    struct tm tm = {0};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;

    time_t t = datetime_mktime(&tm, is_utc);
    XrDateTime *dt = datetime_alloc(isolate);
    dt->timestamp = (int64_t) t;
    dt->milliseconds = 0;
    dt->tz_offset = is_utc ? 0 : xr_datetime_local_offset();
    dt->is_utc = (uint8_t) is_utc;
    return dt;
}

XrDateTime *xr_datetime_from_timestamp(XrayIsolate *isolate, int64_t timestamp) {
    XrDateTime *dt = datetime_alloc(isolate);
    dt->timestamp = timestamp;
    dt->milliseconds = 0;
    dt->tz_offset = 0;
    dt->is_utc = 1;
    return dt;
}

XrDateTime *xr_datetime_from_timestamp_ms(XrayIsolate *isolate, int64_t timestamp_ms) {
    XrDateTime *dt = datetime_alloc(isolate);
    dt->timestamp = timestamp_ms / 1000;
    dt->milliseconds = (int32_t) (timestamp_ms % 1000);
    dt->tz_offset = 0;
    dt->is_utc = 1;
    return dt;
}

/* ========== Parse API ========== */

XrDateTime *xr_datetime_parse(XrayIsolate *isolate, const char *str, const char *format) {
    if (!str)
        return NULL;

    int year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0, ms = 0;

    // Split the input into (date, time, tz) by anchoring on 'T' or ' '
    // first. The previous implementation looked backwards for '+' or '-'
    // and could mis-interpret the '-' used as a date separator in short
    // strings such as "2024-01-15".
    const char *date_end = str;
    while (*date_end && *date_end != 'T' && *date_end != ' ')
        date_end++;
    const char *time_part = NULL;
    if (*date_end == 'T' || *date_end == ' ') {
        time_part = date_end + 1;
    }

    if (!format || strcmp(format, "ISO8601") == 0 || strcmp(format, "iso") == 0) {
        int parsed = sscanf(str, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second);
        // Fall through to space-separated format when the T-format
        // only matched the date part (parsed < 6), e.g. "1979-05-27 07:32:00".
        if (parsed < 6)
            parsed = sscanf(str, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
        if (parsed < 3)
            parsed = sscanf(str, "%d/%d/%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
        if (parsed < 3)
            return NULL;

        // Parse milliseconds (.123) but only when the fractional part
        // follows the time-of-day, never the date. Without this scoping a
        // pattern like "1.2.3" could accidentally contribute to ms.
        const char *dot = time_part ? strchr(time_part, '.') : NULL;
        if (dot && dot[1] >= '0' && dot[1] <= '9') {
            int digits = 0;
            ms = 0;
            const char *p = dot + 1;
            while (*p >= '0' && *p <= '9' && digits < 3) {
                ms = ms * 10 + (*p - '0');
                p++;
                digits++;
            }
            while (digits < 3) {
                ms *= 10;
                digits++;
            }
        }
    } else if (strcmp(format, "date") == 0) {
        if (sscanf(str, "%d-%d-%d", &year, &month, &day) < 3 &&
            sscanf(str, "%d/%d/%d", &year, &month, &day) < 3) {
            return NULL;
        }
    } else if (strcmp(format, "time") == 0) {
        if (sscanf(str, "%d:%d:%d", &hour, &minute, &second) < 2) {
            return NULL;
        }
        year = 1970;
        month = 1;
        day = 1;
    } else {
        if (sscanf(str, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) < 3) {
            return NULL;
        }
    }

    // Parse timezone offset: Z, +HH:MM, -HH:MM. Look only inside the time
    // portion so date separators can never masquerade as the offset sign.
    int is_utc = 0;
    int tz_offset_min = 0;
    if (time_part) {
        const char *scan = time_part;
        const char *tz_marker = NULL;
        while (*scan) {
            if (*scan == 'Z') {
                is_utc = 1;
                break;
            }
            if (*scan == '+' || *scan == '-') {
                tz_marker = scan;
                break;
            }
            scan++;
        }
        if (tz_marker) {
            int tz_h = 0, tz_m = 0;
            if (sscanf(tz_marker + 1, "%d:%d", &tz_h, &tz_m) >= 1) {
                tz_offset_min = tz_h * 60 + tz_m;
                if (*tz_marker == '-')
                    tz_offset_min = -tz_offset_min;
                is_utc = 1;
            }
        }
    }

    struct tm tm = {0};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;

    time_t t;
    if (is_utc) {
        t = datetime_mktime(&tm, 1);
        if (t == (time_t) -1)
            return NULL;
        // Adjust for the timezone offset carried by the parsed string.
        t -= tz_offset_min * 60;
    } else {
        t = mktime(&tm);
        if (t == (time_t) -1)
            return NULL;
    }

    XrDateTime *dt = datetime_alloc(isolate);
    if (!dt)
        return NULL;
    dt->timestamp = (int64_t) t;
    dt->milliseconds = ms;
    dt->tz_offset = is_utc ? 0 : xr_datetime_local_offset();
    dt->is_utc = is_utc;
    return dt;
}

/* ========== Format API ========== */

void xr_datetime_to_tm(XrDateTime *dt, struct tm *tm) {
    time_t t = (time_t) dt->timestamp;
    if (dt->is_utc) {
#ifdef _WIN32
        gmtime_s(tm, &t);
#else
        gmtime_r(&t, tm);
#endif
    } else {
#ifdef _WIN32
        localtime_s(tm, &t);
#else
        localtime_r(&t, tm);
#endif
    }
}

int xr_datetime_format(XrDateTime *dt, const char *pattern, char *buf, size_t buf_size) {
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);

    // Build into an XrCtxBuf so the full formatted output is produced even
    // for very long patterns. We then memcpy the prefix that fits into the
    // caller-provided fixed-size slot, keeping the legacy API contract.
    XrCtxBuf out;
    xr_ctxbuf_init(&out, 64);
    const char *p = pattern;

    while (*p) {
        if (strncmp(p, "YYYY", 4) == 0) {
            xr_ctxbuf_appendf(&out, "%04d", tm.tm_year + 1900);
            p += 4;
        } else if (strncmp(p, "MM", 2) == 0 && p[2] != 'M') {
            xr_ctxbuf_appendf(&out, "%02d", tm.tm_mon + 1);
            p += 2;
        } else if (strncmp(p, "DD", 2) == 0) {
            xr_ctxbuf_appendf(&out, "%02d", tm.tm_mday);
            p += 2;
        } else if (strncmp(p, "HH", 2) == 0) {
            xr_ctxbuf_appendf(&out, "%02d", tm.tm_hour);
            p += 2;
        } else if (strncmp(p, "mm", 2) == 0) {
            xr_ctxbuf_appendf(&out, "%02d", tm.tm_min);
            p += 2;
        } else if (strncmp(p, "ss", 2) == 0) {
            xr_ctxbuf_appendf(&out, "%02d", tm.tm_sec);
            p += 2;
        } else if (strncmp(p, "SSS", 3) == 0) {
            xr_ctxbuf_appendf(&out, "%03d", dt->milliseconds);
            p += 3;
        } else {
            xr_ctxbuf_putc(&out, *p++);
        }
    }

    size_t len = out.len < buf_size - 1 ? out.len : buf_size - 1;
    if (buf_size > 0) {
        if (out.data)
            memcpy(buf, out.data, len);
        buf[len] = '\0';
    }
    xr_ctxbuf_free(&out);
    return (int) len;
}

int xr_datetime_to_iso_string(XrDateTime *dt, char *buf, size_t buf_size) {
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);
    int n;
    if (dt->is_utc) {
        n = snprintf(buf, buf_size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, dt->milliseconds);
    } else {
        int off = dt->tz_offset;
        char sign = off >= 0 ? '+' : '-';
        if (off < 0)
            off = -off;
        n = snprintf(buf, buf_size, "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d:%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                     dt->milliseconds, sign, off / 60, off % 60);
    }
    return n < (int) buf_size ? n : (int) buf_size - 1;
}

/* ========== Component Access API ========== */

int xr_datetime_year(XrDateTime *dt) {
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);
    return tm.tm_year + 1900;
}

int xr_datetime_month(XrDateTime *dt) {
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);
    return tm.tm_mon + 1;
}

int xr_datetime_day(XrDateTime *dt) {
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);
    return tm.tm_mday;
}

int xr_datetime_hour(XrDateTime *dt) {
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);
    return tm.tm_hour;
}

int xr_datetime_minute(XrDateTime *dt) {
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);
    return tm.tm_min;
}

int xr_datetime_second(XrDateTime *dt) {
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);
    return tm.tm_sec;
}

int xr_datetime_millisecond(XrDateTime *dt) {
    return dt->milliseconds;
}

int xr_datetime_weekday(XrDateTime *dt) {
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);
    return tm.tm_wday;
}

int xr_datetime_yearday(XrDateTime *dt) {
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);
    return tm.tm_yday + 1;
}

/* ========== Comparison API ========== */

int xr_datetime_is_before(XrDateTime *dt1, XrDateTime *dt2) {
    if (dt1->timestamp != dt2->timestamp)
        return dt1->timestamp < dt2->timestamp;
    return dt1->milliseconds < dt2->milliseconds;
}

int xr_datetime_is_after(XrDateTime *dt1, XrDateTime *dt2) {
    if (dt1->timestamp != dt2->timestamp)
        return dt1->timestamp > dt2->timestamp;
    return dt1->milliseconds > dt2->milliseconds;
}

int xr_datetime_equals(XrDateTime *dt1, XrDateTime *dt2) {
    return dt1->timestamp == dt2->timestamp && dt1->milliseconds == dt2->milliseconds;
}

/* ========== Utility API ========== */

int xr_datetime_is_leap_year(XrDateTime *dt) {
    int y = xr_datetime_year(dt);
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

int xr_datetime_days_in_month(XrDateTime *dt) {
    return days_in_month_table(xr_datetime_year(dt), xr_datetime_month(dt));
}

/* ========== Date Arithmetic API ========== */

XrDateTime *xr_datetime_add(XrayIsolate *isolate, XrDateTime *dt, int64_t amount,
                            const char *unit) {
    int64_t seconds = 0;

    if (strcmp(unit, "millisecond") == 0 || strcmp(unit, "milliseconds") == 0) {
        XrDateTime *result = datetime_alloc(isolate);
        int64_t total_ms = dt->timestamp * 1000 + dt->milliseconds + amount;
        result->timestamp = total_ms / 1000;
        result->milliseconds = (int32_t) (total_ms % 1000);
        if (result->milliseconds < 0) {
            result->timestamp--;
            result->milliseconds += 1000;
        }
        result->tz_offset = dt->tz_offset;
        result->is_utc = dt->is_utc;
        return result;
    } else if (strcmp(unit, "second") == 0 || strcmp(unit, "seconds") == 0) {
        seconds = amount;
    } else if (strcmp(unit, "minute") == 0 || strcmp(unit, "minutes") == 0) {
        seconds = amount * 60;
    } else if (strcmp(unit, "hour") == 0 || strcmp(unit, "hours") == 0) {
        seconds = amount * 3600;
    } else if (strcmp(unit, "day") == 0 || strcmp(unit, "days") == 0) {
        seconds = amount * 86400;
    } else if (strcmp(unit, "week") == 0 || strcmp(unit, "weeks") == 0) {
        seconds = amount * 604800;
    } else if (strcmp(unit, "month") == 0 || strcmp(unit, "months") == 0) {
        struct tm tm;
        xr_datetime_to_tm(dt, &tm);
        int total_months = (tm.tm_year + 1900) * 12 + tm.tm_mon + (int) amount;
        tm.tm_year = total_months / 12 - 1900;
        tm.tm_mon = total_months % 12;
        if (tm.tm_mon < 0) {
            tm.tm_mon += 12;
            tm.tm_year--;
        }
        // Clamp day to target month's max
        int max_day = days_in_month_table(tm.tm_year + 1900, tm.tm_mon + 1);
        if (tm.tm_mday > max_day)
            tm.tm_mday = max_day;
        tm.tm_isdst = -1;
        time_t t = datetime_mktime(&tm, dt->is_utc);
        XrDateTime *result = datetime_alloc(isolate);
        result->timestamp = (int64_t) t;
        result->milliseconds = dt->milliseconds;
        result->tz_offset = dt->tz_offset;
        result->is_utc = dt->is_utc;
        return result;
    } else if (strcmp(unit, "year") == 0 || strcmp(unit, "years") == 0) {
        struct tm tm;
        xr_datetime_to_tm(dt, &tm);
        tm.tm_year += (int) amount;
        // Clamp day for Feb 29 → non-leap year
        int max_day = days_in_month_table(tm.tm_year + 1900, tm.tm_mon + 1);
        if (tm.tm_mday > max_day)
            tm.tm_mday = max_day;
        tm.tm_isdst = -1;
        time_t t = datetime_mktime(&tm, dt->is_utc);
        XrDateTime *result = datetime_alloc(isolate);
        result->timestamp = (int64_t) t;
        result->milliseconds = dt->milliseconds;
        result->tz_offset = dt->tz_offset;
        result->is_utc = dt->is_utc;
        return result;
    } else {
        // Unknown unit — report error and default to seconds
        fprintf(stderr, "datetime.add(): unknown unit '%s'\n", unit);
        seconds = amount;
    }

    XrDateTime *result = datetime_alloc(isolate);
    result->timestamp = dt->timestamp + seconds;
    result->milliseconds = dt->milliseconds;
    result->tz_offset = dt->tz_offset;
    result->is_utc = dt->is_utc;
    return result;
}

int64_t xr_datetime_diff(XrDateTime *dt1, XrDateTime *dt2, const char *unit) {
    int64_t diff_ms =
        (dt1->timestamp - dt2->timestamp) * 1000 + (dt1->milliseconds - dt2->milliseconds);

    if (strcmp(unit, "millisecond") == 0 || strcmp(unit, "milliseconds") == 0) {
        return diff_ms;
    } else if (strcmp(unit, "second") == 0 || strcmp(unit, "seconds") == 0) {
        return diff_ms / 1000;
    } else if (strcmp(unit, "minute") == 0 || strcmp(unit, "minutes") == 0) {
        return diff_ms / 60000;
    } else if (strcmp(unit, "hour") == 0 || strcmp(unit, "hours") == 0) {
        return diff_ms / 3600000;
    } else if (strcmp(unit, "day") == 0 || strcmp(unit, "days") == 0) {
        return diff_ms / 86400000;
    } else if (strcmp(unit, "week") == 0 || strcmp(unit, "weeks") == 0) {
        return diff_ms / 604800000;
    }
    // Default: seconds
    return diff_ms / 1000;
}

/* ========== Timezone API ========== */

// Copy every user-visible field explicitly instead of doing a raw memcpy
// past the GC header: the old byte-level copy silently broke every time
// XrGCHeader changed layout, and produced subtle aliasing issues when the
// GC added mark bits to the header.
static void datetime_copy_fields(XrDateTime *dst, const XrDateTime *src) {
    dst->timestamp = src->timestamp;
    dst->milliseconds = src->milliseconds;
    dst->tz_offset = src->tz_offset;
    dst->is_utc = src->is_utc;
}

XrDateTime *xr_datetime_to_utc(XrayIsolate *isolate, XrDateTime *dt) {
    XrDateTime *result = datetime_alloc(isolate);
    if (!result)
        return NULL;
    if (dt->is_utc) {
        datetime_copy_fields(result, dt);
    } else {
        result->timestamp = dt->timestamp;
        result->milliseconds = dt->milliseconds;
        result->tz_offset = 0;
        result->is_utc = 1;
    }
    return result;
}

XrDateTime *xr_datetime_to_local(XrayIsolate *isolate, XrDateTime *dt) {
    XrDateTime *result = datetime_alloc(isolate);
    if (!result)
        return NULL;
    if (!dt->is_utc) {
        datetime_copy_fields(result, dt);
    } else {
        result->timestamp = dt->timestamp;
        result->milliseconds = dt->milliseconds;
        result->tz_offset = xr_datetime_local_offset();
        result->is_utc = 0;
    }
    return result;
}

/* ========== Module Binding Functions ========== */

// All binding functions are static, used by both module export AND native type method tables.
// For methods: args[0] = self (DateTime), args[1..] = method args
// For module functions: args[0..] = function args

static XrValue dt_now(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    return xr_datetime_value(xr_datetime_now(isolate));
}

static XrValue dt_utc(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    return xr_datetime_value(xr_datetime_utc(isolate));
}

static XrValue dt_create(XrayIsolate *isolate, XrValue *args, int nargs) {
    int year = nargs > 0 && XR_IS_INT(args[0]) ? (int) XR_TO_INT(args[0]) : 1970;
    int month = nargs > 1 && XR_IS_INT(args[1]) ? (int) XR_TO_INT(args[1]) : 1;
    int day = nargs > 2 && XR_IS_INT(args[2]) ? (int) XR_TO_INT(args[2]) : 1;
    int hour = nargs > 3 && XR_IS_INT(args[3]) ? (int) XR_TO_INT(args[3]) : 0;
    int minute = nargs > 4 && XR_IS_INT(args[4]) ? (int) XR_TO_INT(args[4]) : 0;
    int second = nargs > 5 && XR_IS_INT(args[5]) ? (int) XR_TO_INT(args[5]) : 0;
    return xr_datetime_value(
        xr_datetime_create(isolate, year, month, day, hour, minute, second, 0));
}

static XrValue dt_create_utc(XrayIsolate *isolate, XrValue *args, int nargs) {
    int year = nargs > 0 && XR_IS_INT(args[0]) ? (int) XR_TO_INT(args[0]) : 1970;
    int month = nargs > 1 && XR_IS_INT(args[1]) ? (int) XR_TO_INT(args[1]) : 1;
    int day = nargs > 2 && XR_IS_INT(args[2]) ? (int) XR_TO_INT(args[2]) : 1;
    int hour = nargs > 3 && XR_IS_INT(args[3]) ? (int) XR_TO_INT(args[3]) : 0;
    int minute = nargs > 4 && XR_IS_INT(args[4]) ? (int) XR_TO_INT(args[4]) : 0;
    int second = nargs > 5 && XR_IS_INT(args[5]) ? (int) XR_TO_INT(args[5]) : 0;
    return xr_datetime_value(
        xr_datetime_create(isolate, year, month, day, hour, minute, second, 1));
}

static XrValue dt_from_timestamp(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1)
        return XR_NULL_VAL;
    int64_t ts = 0;
    if (XR_IS_INT(args[0]))
        ts = XR_TO_INT(args[0]);
    else if (XR_IS_FLOAT(args[0]))
        ts = (int64_t) XR_TO_FLOAT(args[0]);
    else
        return XR_NULL_VAL;
    return xr_datetime_value(xr_datetime_from_timestamp(isolate, ts));
}

static XrValue dt_from_timestamp_ms(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1)
        return XR_NULL_VAL;
    int64_t ts = 0;
    if (XR_IS_INT(args[0]))
        ts = XR_TO_INT(args[0]);
    else if (XR_IS_FLOAT(args[0]))
        ts = (int64_t) XR_TO_FLOAT(args[0]);
    else
        return XR_NULL_VAL;
    return xr_datetime_value(xr_datetime_from_timestamp_ms(isolate, ts));
}

static XrValue dt_parse(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0]))
        return XR_NULL_VAL;
    XrString *str = XR_TO_STRING(args[0]);
    const char *format = NULL;
    if (nargs > 1 && XR_IS_STRING(args[1])) {
        format = XR_STRING_CHARS(XR_TO_STRING(args[1]));
    }
    XrDateTime *dt = xr_datetime_parse(isolate, XR_STRING_CHARS(str), format);
    return dt ? xr_datetime_value(dt) : XR_NULL_VAL;
}

static XrValue dt_offset(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return XR_INT(xr_datetime_local_offset());
}

// Method binding: args[0] = self (DateTime)

static XrValue dt_format(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_NULL_VAL;
    XrDateTime *dt = XR_TO_DATETIME(args[0]);
    const char *pattern = "YYYY-MM-DD HH:mm:ss";
    if (nargs > 1 && XR_IS_STRING(args[1])) {
        pattern = XR_STRING_CHARS(XR_TO_STRING(args[1]));
    }

    // Build into a dynamic buffer so long custom patterns (e.g. embedding
    // localized strings) are never silently truncated.
    XrCtxBuf out;
    xr_ctxbuf_init(&out, 64);
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);
    for (const char *p = pattern; *p;) {
        if (strncmp(p, "YYYY", 4) == 0) {
            xr_ctxbuf_appendf(&out, "%04d", tm.tm_year + 1900);
            p += 4;
        } else if (strncmp(p, "MM", 2) == 0 && p[2] != 'M') {
            xr_ctxbuf_appendf(&out, "%02d", tm.tm_mon + 1);
            p += 2;
        } else if (strncmp(p, "DD", 2) == 0) {
            xr_ctxbuf_appendf(&out, "%02d", tm.tm_mday);
            p += 2;
        } else if (strncmp(p, "HH", 2) == 0) {
            xr_ctxbuf_appendf(&out, "%02d", tm.tm_hour);
            p += 2;
        } else if (strncmp(p, "mm", 2) == 0) {
            xr_ctxbuf_appendf(&out, "%02d", tm.tm_min);
            p += 2;
        } else if (strncmp(p, "ss", 2) == 0) {
            xr_ctxbuf_appendf(&out, "%02d", tm.tm_sec);
            p += 2;
        } else if (strncmp(p, "SSS", 3) == 0) {
            xr_ctxbuf_appendf(&out, "%03d", dt->milliseconds);
            p += 3;
        } else {
            xr_ctxbuf_putc(&out, *p++);
        }
    }
    XrValue v = xr_string_value(xr_string_new(isolate, out.data ? out.data : "", out.len));
    xr_ctxbuf_free(&out);
    return v;
}

static XrValue dt_to_iso(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_NULL_VAL;
    char buf[64];
    int len = xr_datetime_to_iso_string(XR_TO_DATETIME(args[0]), buf, sizeof(buf));
    return xr_string_value(xr_string_new(isolate, buf, len));
}

static XrValue dt_year(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_INT(0);
    return XR_INT(xr_datetime_year(XR_TO_DATETIME(args[0])));
}

static XrValue dt_month(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_INT(0);
    return XR_INT(xr_datetime_month(XR_TO_DATETIME(args[0])));
}

static XrValue dt_day(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_INT(0);
    return XR_INT(xr_datetime_day(XR_TO_DATETIME(args[0])));
}

static XrValue dt_hour(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_INT(0);
    return XR_INT(xr_datetime_hour(XR_TO_DATETIME(args[0])));
}

static XrValue dt_minute(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_INT(0);
    return XR_INT(xr_datetime_minute(XR_TO_DATETIME(args[0])));
}

static XrValue dt_second(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_INT(0);
    return XR_INT(xr_datetime_second(XR_TO_DATETIME(args[0])));
}

static XrValue dt_millisecond(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_INT(0);
    return XR_INT(xr_datetime_millisecond(XR_TO_DATETIME(args[0])));
}

static XrValue dt_weekday(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_INT(0);
    return XR_INT(xr_datetime_weekday(XR_TO_DATETIME(args[0])));
}

static XrValue dt_yearday(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_INT(0);
    return XR_INT(xr_datetime_yearday(XR_TO_DATETIME(args[0])));
}

static XrValue dt_timestamp(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_INT(0);
    return XR_INT(XR_TO_DATETIME(args[0])->timestamp);
}

static XrValue dt_add(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 3 || !XR_IS_DATETIME(args[0]) || !XR_IS_STRING(args[2]))
        return XR_NULL_VAL;
    XrDateTime *dt = XR_TO_DATETIME(args[0]);
    int64_t amount = XR_IS_INT(args[1]) ? XR_TO_INT(args[1]) : 0;
    const char *unit = XR_STRING_CHARS(XR_TO_STRING(args[2]));
    return xr_datetime_value(xr_datetime_add(isolate, dt, amount, unit));
}

static XrValue dt_diff(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 2 || !XR_IS_DATETIME(args[0]) || !XR_IS_DATETIME(args[1]))
        return XR_INT(0);
    XrDateTime *dt1 = XR_TO_DATETIME(args[0]);
    XrDateTime *dt2 = XR_TO_DATETIME(args[1]);
    const char *unit = "seconds";
    if (nargs > 2 && XR_IS_STRING(args[2])) {
        unit = XR_STRING_CHARS(XR_TO_STRING(args[2]));
    }
    return XR_INT(xr_datetime_diff(dt1, dt2, unit));
}

static XrValue dt_to_utc(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_NULL_VAL;
    return xr_datetime_value(xr_datetime_to_utc(isolate, XR_TO_DATETIME(args[0])));
}

static XrValue dt_to_local(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_NULL_VAL;
    return xr_datetime_value(xr_datetime_to_local(isolate, XR_TO_DATETIME(args[0])));
}

static XrValue dt_is_before(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 2 || !XR_IS_DATETIME(args[0]) || !XR_IS_DATETIME(args[1]))
        return XR_FALSE_VAL;
    return xr_datetime_is_before(XR_TO_DATETIME(args[0]), XR_TO_DATETIME(args[1])) ? XR_TRUE_VAL
                                                                                   : XR_FALSE_VAL;
}

static XrValue dt_is_after(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 2 || !XR_IS_DATETIME(args[0]) || !XR_IS_DATETIME(args[1]))
        return XR_FALSE_VAL;
    return xr_datetime_is_after(XR_TO_DATETIME(args[0]), XR_TO_DATETIME(args[1])) ? XR_TRUE_VAL
                                                                                  : XR_FALSE_VAL;
}

static XrValue dt_equals(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 2 || !XR_IS_DATETIME(args[0]) || !XR_IS_DATETIME(args[1]))
        return XR_FALSE_VAL;
    return xr_datetime_equals(XR_TO_DATETIME(args[0]), XR_TO_DATETIME(args[1])) ? XR_TRUE_VAL
                                                                                : XR_FALSE_VAL;
}

static XrValue dt_is_leap_year(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_FALSE_VAL;
    return xr_datetime_is_leap_year(XR_TO_DATETIME(args[0])) ? XR_TRUE_VAL : XR_FALSE_VAL;
}

static XrValue dt_days_in_month(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1 || !XR_IS_DATETIME(args[0]))
        return XR_INT(0);
    return XR_INT(xr_datetime_days_in_month(XR_TO_DATETIME(args[0])));
}

#include "../../src/runtime/object/xnative_type.h"

/* ========== DateTime Native Type Method Table ========== */

static XrNativeMethod datetime_methods[] = {{"format", (XrCFunctionPtr) dt_format, 2},
                                            {"toISOString", (XrCFunctionPtr) dt_to_iso, 1},
                                            {"add", (XrCFunctionPtr) dt_add, 3},
                                            {"diff", (XrCFunctionPtr) dt_diff, 3},
                                            {"toUTC", (XrCFunctionPtr) dt_to_utc, 1},
                                            {"toLocal", (XrCFunctionPtr) dt_to_local, 1},
                                            {"isBefore", (XrCFunctionPtr) dt_is_before, 2},
                                            {"isAfter", (XrCFunctionPtr) dt_is_after, 2},
                                            {"equals", (XrCFunctionPtr) dt_equals, 2},
                                            {"isLeapYear", (XrCFunctionPtr) dt_is_leap_year, 1},
                                            {"daysInMonth", (XrCFunctionPtr) dt_days_in_month, 1},
                                            {NULL, NULL, 0}};

static XrNativeMethod datetime_getters[] = {{"year", (XrCFunctionPtr) dt_year, 1},
                                            {"month", (XrCFunctionPtr) dt_month, 1},
                                            {"day", (XrCFunctionPtr) dt_day, 1},
                                            {"hour", (XrCFunctionPtr) dt_hour, 1},
                                            {"minute", (XrCFunctionPtr) dt_minute, 1},
                                            {"second", (XrCFunctionPtr) dt_second, 1},
                                            {"millisecond", (XrCFunctionPtr) dt_millisecond, 1},
                                            {"weekday", (XrCFunctionPtr) dt_weekday, 1},
                                            {"yearday", (XrCFunctionPtr) dt_yearday, 1},
                                            {"timestamp", (XrCFunctionPtr) dt_timestamp, 1},
                                            {NULL, NULL, 0}};

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module datetime

XR_DEFINE_BUILTIN(dt_now, "now", "(): DateTime", "Get current local datetime")
XR_DEFINE_BUILTIN(dt_utc, "utc", "(): DateTime", "Get current UTC datetime")
XR_DEFINE_BUILTIN(
    dt_create, "create",
    "(year: int, month?: int, day?: int, hour?: int, minute?: int, second?: int): DateTime",
    "Create local datetime")
XR_DEFINE_BUILTIN(
    dt_create_utc, "createUTC",
    "(year: int, month?: int, day?: int, hour?: int, minute?: int, second?: int): DateTime",
    "Create UTC datetime")
XR_DEFINE_BUILTIN(dt_from_timestamp, "fromTimestamp", "(ts: int): DateTime",
                  "Create datetime from Unix timestamp (seconds)")
XR_DEFINE_BUILTIN(dt_from_timestamp_ms, "fromTimestampMs", "(ts: int): DateTime",
                  "Create datetime from Unix timestamp (milliseconds)")
XR_DEFINE_BUILTIN(dt_parse, "parse", "(s: string, format?: string): DateTime?",
                  "Parse datetime string")
XR_DEFINE_BUILTIN(dt_offset, "offset", "(): int", "Get UTC offset in minutes")

// @type DateTime
XR_DEFINE_BUILTIN(dt_format, "format", "(pattern?: string): string", "Format datetime to string")
XR_DEFINE_BUILTIN(dt_to_iso, "toISOString", "(): string", "Convert to ISO 8601 string")
XR_DEFINE_BUILTIN(dt_year, "year", "(): int", "Get year")
XR_DEFINE_BUILTIN(dt_month, "month", "(): int", "Get month (1-12)")
XR_DEFINE_BUILTIN(dt_day, "day", "(): int", "Get day (1-31)")
XR_DEFINE_BUILTIN(dt_hour, "hour", "(): int", "Get hour (0-23)")
XR_DEFINE_BUILTIN(dt_minute, "minute", "(): int", "Get minute (0-59)")
XR_DEFINE_BUILTIN(dt_second, "second", "(): int", "Get second (0-59)")
XR_DEFINE_BUILTIN(dt_millisecond, "millisecond", "(): int", "Get millisecond (0-999)")
XR_DEFINE_BUILTIN(dt_weekday, "weekday", "(): int", "Get weekday (0=Sunday)")
XR_DEFINE_BUILTIN(dt_yearday, "yearday", "(): int", "Get day of year (1-366)")
XR_DEFINE_BUILTIN(dt_timestamp, "timestamp", "(): int", "Get Unix timestamp (seconds)")
XR_DEFINE_BUILTIN(dt_add, "add", "(amount: int, unit: string): DateTime",
                  "Add duration to datetime")
XR_DEFINE_BUILTIN(dt_diff, "diff", "(other: DateTime, unit?: string): int",
                  "Difference between datetimes")
XR_DEFINE_BUILTIN(dt_to_utc, "toUTC", "(): DateTime", "Convert to UTC")
XR_DEFINE_BUILTIN(dt_to_local, "toLocal", "(): DateTime", "Convert to local time")
XR_DEFINE_BUILTIN(dt_is_before, "isBefore", "(other: DateTime): bool",
                  "Check if before other datetime")
XR_DEFINE_BUILTIN(dt_is_after, "isAfter", "(other: DateTime): bool",
                  "Check if after other datetime")
XR_DEFINE_BUILTIN(dt_equals, "equals", "(other: DateTime): bool",
                  "Check if equal to other datetime")
XR_DEFINE_BUILTIN(dt_is_leap_year, "isLeapYear", "(): bool", "Check if leap year")
XR_DEFINE_BUILTIN(dt_days_in_month, "daysInMonth", "(): int", "Get days in current month")

XrModule *xr_load_module_datetime(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_datetime: NULL isolate");

    // Register DateTime native type
    XrNativeTypeInfo dt_info = {
        .name = "DateTime",
        .gc_type = XR_TDATETIME,
        .methods = datetime_methods,
        .getters = datetime_getters,
        .static_methods = NULL,
    };
    xr_register_native_type(isolate, &dt_info);

    // Create module — only factory functions exported
    XrModule *mod = xr_module_create_native(isolate, "datetime");
    if (!mod)
        return NULL;

    XRS_EXPORT(mod, isolate, "now", dt_now);
    XRS_EXPORT(mod, isolate, "utc", dt_utc);
    XRS_EXPORT(mod, isolate, "create", dt_create);
    XRS_EXPORT(mod, isolate, "createUTC", dt_create_utc);
    XRS_EXPORT(mod, isolate, "fromTimestamp", dt_from_timestamp);
    XRS_EXPORT(mod, isolate, "fromTimestampMs", dt_from_timestamp_ms);
    XRS_EXPORT(mod, isolate, "parse", dt_parse);
    XRS_EXPORT(mod, isolate, "offset", dt_offset);

    mod->loaded = true;
    return mod;
}
