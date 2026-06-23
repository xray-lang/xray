/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_datetime.h - Freestanding AOT DateTime light object.
 */

#ifndef XRT_DATETIME_H
#define XRT_DATETIME_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../shared/xr_datetime_core.h"
#if defined(XR_OS_POSIX)
#include <sys/time.h>
#endif
#include "xrt_arc.h"
#include "xrt_method_symbols.h"
#include "xrt_value.h"

typedef struct xrt_datetime_object {
    int64_t timestamp;
    int32_t milliseconds;
    int32_t tz_offset;
    uint8_t is_utc;
    uint8_t _pad[7];
} xrt_datetime_object_t;

static inline int xrt_datetime_is(XrValue v) {
    return v.tag == XR_TAG_DATETIME && v.ptr != NULL;
}

static inline xrt_datetime_object_t *xrt_datetime_ptr(XrValue v) {
    return xrt_datetime_is(v) ? (xrt_datetime_object_t *) v.ptr : NULL;
}

static inline XrValue xrt_datetime_box(xrt_datetime_object_t *dt) {
    return dt ? xr_mkptr(dt, XR_TAG_DATETIME) : XR_NULL_VAL;
}

static inline xrt_datetime_object_t *xrt_datetime_alloc(void) {
    xrt_datetime_object_t *dt = (xrt_datetime_object_t *) xrt_arc_alloc(sizeof(*dt));
    dt->timestamp = 0;
    dt->milliseconds = 0;
    dt->tz_offset = 0;
    dt->is_utc = 0;
    memset(dt->_pad, 0, sizeof(dt->_pad));
    return dt;
}

static inline int64_t xrt_datetime_i64_arg(XrValue v, int64_t fallback) {
    if (XR_IS_INT(v))
        return XR_TO_INT(v);
    if (XR_IS_FLOAT(v))
        return (int64_t) XR_TO_FLOAT(v);
    if (XR_IS_BOOL(v))
        return v.i ? 1 : 0;
    return fallback;
}

static inline int64_t xrt_datetime_current_millis(void) {
#if defined(XR_OS_POSIX)
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0)
        return (int64_t) tv.tv_sec * 1000 + (int64_t) tv.tv_usec / 1000;
#endif
    return (int64_t) time(NULL) * 1000;
}

static inline void xrt_datetime_to_tm_obj(const xrt_datetime_object_t *dt, struct tm *tmv) {
    time_t t = (time_t) dt->timestamp;
    if (!dt->is_utc)
        t += (time_t) dt->tz_offset * 60;
    xr_datetime_core_gmtime(t, tmv);
}

static inline XrValue xrt_datetime_make(int year, int month, int day, int hour, int minute,
                                        int second, int is_utc) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = year - 1900;
    tmv.tm_mon = month - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hour;
    tmv.tm_min = minute;
    tmv.tm_sec = second;
    tmv.tm_isdst = -1;

    time_t t = xr_datetime_core_mktime(&tmv, is_utc);
    xrt_datetime_object_t *dt = xrt_datetime_alloc();
    dt->timestamp = (int64_t) t;
    dt->milliseconds = 0;
    dt->tz_offset = is_utc ? 0 : xr_datetime_core_local_offset_at(t);
    dt->is_utc = (uint8_t) (is_utc ? 1 : 0);
    return xrt_datetime_box(dt);
}

static inline XrValue xrt_datetime_make_defaulted(XrValue year_v, XrValue month_v, XrValue day_v,
                                                  XrValue hour_v, XrValue minute_v,
                                                  XrValue second_v, int argc, int is_utc) {
    int year = (int) xrt_datetime_i64_arg(year_v, 1970);
    int month = argc > 1 ? (int) xrt_datetime_i64_arg(month_v, 1) : 1;
    int day = argc > 2 ? (int) xrt_datetime_i64_arg(day_v, 1) : 1;
    int hour = argc > 3 ? (int) xrt_datetime_i64_arg(hour_v, 0) : 0;
    int minute = argc > 4 ? (int) xrt_datetime_i64_arg(minute_v, 0) : 0;
    int second = argc > 5 ? (int) xrt_datetime_i64_arg(second_v, 0) : 0;
    return xrt_datetime_make(year, month, day, hour, minute, second, is_utc);
}

static inline XrValue xrt_datetime_create_1(XrValue y) {
    return xrt_datetime_make_defaulted(y, XR_NULL_VAL, XR_NULL_VAL, XR_NULL_VAL, XR_NULL_VAL,
                                       XR_NULL_VAL, 1, 0);
}

static inline XrValue xrt_datetime_create_2(XrValue y, XrValue mo) {
    return xrt_datetime_make_defaulted(y, mo, XR_NULL_VAL, XR_NULL_VAL, XR_NULL_VAL, XR_NULL_VAL, 2,
                                       0);
}

static inline XrValue xrt_datetime_create_3(XrValue y, XrValue mo, XrValue d) {
    return xrt_datetime_make_defaulted(y, mo, d, XR_NULL_VAL, XR_NULL_VAL, XR_NULL_VAL, 3, 0);
}

static inline XrValue xrt_datetime_create_4(XrValue y, XrValue mo, XrValue d, XrValue h) {
    return xrt_datetime_make_defaulted(y, mo, d, h, XR_NULL_VAL, XR_NULL_VAL, 4, 0);
}

static inline XrValue xrt_datetime_create_5(XrValue y, XrValue mo, XrValue d, XrValue h,
                                            XrValue mi) {
    return xrt_datetime_make_defaulted(y, mo, d, h, mi, XR_NULL_VAL, 5, 0);
}

static inline XrValue xrt_datetime_create_6(XrValue y, XrValue mo, XrValue d, XrValue h, XrValue mi,
                                            XrValue s) {
    return xrt_datetime_make_defaulted(y, mo, d, h, mi, s, 6, 0);
}

static inline XrValue xrt_datetime_create_utc_1(XrValue y) {
    return xrt_datetime_make_defaulted(y, XR_NULL_VAL, XR_NULL_VAL, XR_NULL_VAL, XR_NULL_VAL,
                                       XR_NULL_VAL, 1, 1);
}

static inline XrValue xrt_datetime_create_utc_2(XrValue y, XrValue mo) {
    return xrt_datetime_make_defaulted(y, mo, XR_NULL_VAL, XR_NULL_VAL, XR_NULL_VAL, XR_NULL_VAL, 2,
                                       1);
}

static inline XrValue xrt_datetime_create_utc_3(XrValue y, XrValue mo, XrValue d) {
    return xrt_datetime_make_defaulted(y, mo, d, XR_NULL_VAL, XR_NULL_VAL, XR_NULL_VAL, 3, 1);
}

static inline XrValue xrt_datetime_create_utc_4(XrValue y, XrValue mo, XrValue d, XrValue h) {
    return xrt_datetime_make_defaulted(y, mo, d, h, XR_NULL_VAL, XR_NULL_VAL, 4, 1);
}

static inline XrValue xrt_datetime_create_utc_5(XrValue y, XrValue mo, XrValue d, XrValue h,
                                                XrValue mi) {
    return xrt_datetime_make_defaulted(y, mo, d, h, mi, XR_NULL_VAL, 5, 1);
}

static inline XrValue xrt_datetime_create_utc_6(XrValue y, XrValue mo, XrValue d, XrValue h,
                                                XrValue mi, XrValue s) {
    return xrt_datetime_make_defaulted(y, mo, d, h, mi, s, 6, 1);
}

static inline XrValue xrt_datetime_now(void) {
    int64_t millis = xrt_datetime_current_millis();
    time_t t = (time_t) (millis / 1000);
    xrt_datetime_object_t *dt = xrt_datetime_alloc();
    dt->timestamp = (int64_t) t;
    dt->milliseconds = (int32_t) (millis % 1000);
    dt->tz_offset = xr_datetime_core_local_offset_at(t);
    dt->is_utc = 0;
    return xrt_datetime_box(dt);
}

static inline XrValue xrt_datetime_utc(void) {
    int64_t millis = xrt_datetime_current_millis();
    xrt_datetime_object_t *dt = xrt_datetime_alloc();
    dt->timestamp = millis / 1000;
    dt->milliseconds = (int32_t) (millis % 1000);
    dt->tz_offset = 0;
    dt->is_utc = 1;
    return xrt_datetime_box(dt);
}

static inline XrValue xrt_datetime_from_timestamp(XrValue timestamp) {
    xrt_datetime_object_t *dt = xrt_datetime_alloc();
    dt->timestamp = xrt_datetime_i64_arg(timestamp, 0);
    dt->milliseconds = 0;
    dt->tz_offset = 0;
    dt->is_utc = 1;
    return xrt_datetime_box(dt);
}

static inline XrValue xrt_datetime_from_timestamp_ms(XrValue timestamp_ms) {
    int64_t total_ms = xrt_datetime_i64_arg(timestamp_ms, 0);
    int64_t sec = total_ms / 1000;
    int32_t ms = (int32_t) (total_ms % 1000);
    if (ms < 0) {
        sec -= 1;
        ms += 1000;
    }
    xrt_datetime_object_t *dt = xrt_datetime_alloc();
    dt->timestamp = sec;
    dt->milliseconds = ms;
    dt->tz_offset = 0;
    dt->is_utc = 1;
    return xrt_datetime_box(dt);
}

static inline XrValue xrt_datetime_offset(void) {
    return XR_FROM_INT((int64_t) xr_datetime_core_local_offset_at(time(NULL)));
}

static inline XrValue xrt_datetime_parse_impl(const char *str, const char *format) {
    if (!str)
        return XR_NULL_VAL;

    int year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0, ms = 0;
    const char *date_end = str;
    while (*date_end && *date_end != 'T' && *date_end != ' ')
        date_end++;
    const char *time_part = (*date_end == 'T' || *date_end == ' ') ? date_end + 1 : NULL;

    if (!format || strcmp(format, "ISO8601") == 0 || strcmp(format, "iso") == 0) {
        int parsed = sscanf(str, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second);
        if (parsed < 6)
            parsed = sscanf(str, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
        if (parsed < 3)
            parsed = sscanf(str, "%d/%d/%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
        if (parsed < 3)
            return XR_NULL_VAL;

        const char *dot = time_part ? strchr(time_part, '.') : NULL;
        if (dot && dot[1] >= '0' && dot[1] <= '9') {
            int digits = 0;
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
            sscanf(str, "%d/%d/%d", &year, &month, &day) < 3)
            return XR_NULL_VAL;
    } else if (strcmp(format, "time") == 0) {
        if (sscanf(str, "%d:%d:%d", &hour, &minute, &second) < 2)
            return XR_NULL_VAL;
        year = 1970;
        month = 1;
        day = 1;
    } else {
        if (sscanf(str, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) < 3)
            return XR_NULL_VAL;
    }

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

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = year - 1900;
    tmv.tm_mon = month - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hour;
    tmv.tm_min = minute;
    tmv.tm_sec = second;
    tmv.tm_isdst = -1;

    time_t t = xr_datetime_core_mktime(&tmv, is_utc);
    if (is_utc)
        t -= tz_offset_min * 60;

    xrt_datetime_object_t *dt = xrt_datetime_alloc();
    dt->timestamp = (int64_t) t;
    dt->milliseconds = ms;
    dt->tz_offset = is_utc ? 0 : xr_datetime_core_local_offset_at(t);
    dt->is_utc = (uint8_t) is_utc;
    return xrt_datetime_box(dt);
}

static inline XrValue xrt_datetime_parse_default(const char *data, int64_t len) {
    (void) len;
    return xrt_datetime_parse_impl(data, NULL);
}

static inline XrValue xrt_datetime_parse_format(const char *data, int64_t len,
                                                const char *format_data, int64_t format_len) {
    (void) len;
    (void) format_len;
    return xrt_datetime_parse_impl(data, format_data);
}

static inline int xrt_datetime_year_ptr(const xrt_datetime_object_t *dt) {
    struct tm tmv;
    xrt_datetime_to_tm_obj(dt, &tmv);
    return tmv.tm_year + 1900;
}

static inline int xrt_datetime_month_ptr(const xrt_datetime_object_t *dt) {
    struct tm tmv;
    xrt_datetime_to_tm_obj(dt, &tmv);
    return tmv.tm_mon + 1;
}

static inline int xrt_datetime_day_ptr(const xrt_datetime_object_t *dt) {
    struct tm tmv;
    xrt_datetime_to_tm_obj(dt, &tmv);
    return tmv.tm_mday;
}

static inline int xrt_datetime_hour_ptr(const xrt_datetime_object_t *dt) {
    struct tm tmv;
    xrt_datetime_to_tm_obj(dt, &tmv);
    return tmv.tm_hour;
}

static inline int xrt_datetime_minute_ptr(const xrt_datetime_object_t *dt) {
    struct tm tmv;
    xrt_datetime_to_tm_obj(dt, &tmv);
    return tmv.tm_min;
}

static inline int xrt_datetime_second_ptr(const xrt_datetime_object_t *dt) {
    struct tm tmv;
    xrt_datetime_to_tm_obj(dt, &tmv);
    return tmv.tm_sec;
}

static inline int xrt_datetime_weekday_ptr(const xrt_datetime_object_t *dt) {
    struct tm tmv;
    xrt_datetime_to_tm_obj(dt, &tmv);
    return tmv.tm_wday;
}

static inline int xrt_datetime_yearday_ptr(const xrt_datetime_object_t *dt) {
    struct tm tmv;
    xrt_datetime_to_tm_obj(dt, &tmv);
    return tmv.tm_yday + 1;
}

static inline int xrt_datetime_is_before_ptr(const xrt_datetime_object_t *a,
                                             const xrt_datetime_object_t *b) {
    if (!a || !b)
        return 0;
    if (a->timestamp != b->timestamp)
        return a->timestamp < b->timestamp;
    return a->milliseconds < b->milliseconds;
}

static inline int xrt_datetime_is_after_ptr(const xrt_datetime_object_t *a,
                                            const xrt_datetime_object_t *b) {
    if (!a || !b)
        return 0;
    if (a->timestamp != b->timestamp)
        return a->timestamp > b->timestamp;
    return a->milliseconds > b->milliseconds;
}

static inline int xrt_datetime_equals_ptr(const xrt_datetime_object_t *a,
                                          const xrt_datetime_object_t *b) {
    return a && b && a->timestamp == b->timestamp && a->milliseconds == b->milliseconds;
}

typedef struct xrt_datetime_buffer_writer {
    char *data;
    size_t cap;
    size_t len;
} xrt_datetime_buffer_writer_t;

static inline int xrt_datetime_buffer_write(void *ctx, const char *data, size_t len) {
    xrt_datetime_buffer_writer_t *writer = (xrt_datetime_buffer_writer_t *) ctx;
    if (!writer || !writer->data || (!data && len != 0))
        return -1;
    if (writer->len > writer->cap || len > writer->cap - writer->len)
        return -1;
    if (len != 0)
        memcpy(writer->data + writer->len, data, len);
    writer->len += len;
    return 0;
}

static inline XrValue xrt_datetime_format_pattern(const xrt_datetime_object_t *dt,
                                                  const char *pattern, int64_t pattern_len) {
    if (!dt || !pattern)
        return XR_NULL_VAL;
    if (pattern_len < 0)
        pattern_len = (int64_t) strlen(pattern);
    size_t cap = (size_t) pattern_len * 4u + 32u;
    XrValue out = xrt_str_alloc(cap);
    struct tm tmv;
    xrt_datetime_to_tm_obj(dt, &tmv);
    xrt_datetime_buffer_writer_t buffer = {.data = xr_str_buf(out), .cap = cap, .len = 0};
    XrDateTimeCoreWriter writer = {.ctx = &buffer, .write = xrt_datetime_buffer_write};
    if (xr_datetime_core_format_tm(&writer, &tmv, dt->milliseconds, pattern, pattern_len) < 0) {
        xrt_release(out);
        return XR_NULL_VAL;
    }
    buffer.data[buffer.len] = '\0';
    xr_str_hdr(out)->len = (int64_t) buffer.len;
    return out;
}

static inline XrValue xrt_datetime_to_string_value(const xrt_datetime_object_t *dt) {
    return xrt_datetime_format_pattern(dt, "YYYY-MM-DD HH:mm:ss", 19);
}

static inline XrValue xrt_datetime_to_iso_value(const xrt_datetime_object_t *dt) {
    if (!dt)
        return XR_NULL_VAL;
    struct tm tmv;
    xrt_datetime_to_tm_obj(dt, &tmv);
    XrValue out = xrt_str_alloc(95);
    xrt_datetime_buffer_writer_t buffer = {.data = xr_str_buf(out), .cap = 95, .len = 0};
    XrDateTimeCoreWriter writer = {.ctx = &buffer, .write = xrt_datetime_buffer_write};
    if (xr_datetime_core_iso_write(&writer, &tmv, dt->milliseconds, dt->is_utc, dt->tz_offset) <
        0) {
        xrt_release(out);
        return XR_NULL_VAL;
    }
    buffer.data[buffer.len] = '\0';
    xr_str_hdr(out)->len = (int64_t) buffer.len;
    return out;
}

static inline XrValue xrt_datetime_add_value(XrValue recv, XrValue amount_v, XrValue unit_v) {
    const xrt_datetime_object_t *dt = xrt_datetime_ptr(recv);
    if (!dt || !XR_IS_STR(unit_v))
        return XR_NULL_VAL;
    int64_t amount = xrt_datetime_i64_arg(amount_v, 0);
    const char *unit = xr_str_data(unit_v);
    int64_t seconds = 0;

    if (strcmp(unit, "millisecond") == 0 || strcmp(unit, "milliseconds") == 0) {
        int64_t total_ms = dt->timestamp * 1000 + dt->milliseconds + amount;
        xrt_datetime_object_t *result = xrt_datetime_alloc();
        result->timestamp = total_ms / 1000;
        result->milliseconds = (int32_t) (total_ms % 1000);
        if (result->milliseconds < 0) {
            result->timestamp--;
            result->milliseconds += 1000;
        }
        result->tz_offset = dt->tz_offset;
        result->is_utc = dt->is_utc;
        return xrt_datetime_box(result);
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
        struct tm tmv;
        xrt_datetime_to_tm_obj(dt, &tmv);
        int total_months = (tmv.tm_year + 1900) * 12 + tmv.tm_mon + (int) amount;
        tmv.tm_year = total_months / 12 - 1900;
        tmv.tm_mon = total_months % 12;
        if (tmv.tm_mon < 0) {
            tmv.tm_mon += 12;
            tmv.tm_year--;
        }
        int max_day = xr_datetime_core_days_in_month(tmv.tm_year + 1900, tmv.tm_mon + 1);
        if (tmv.tm_mday > max_day)
            tmv.tm_mday = max_day;
        tmv.tm_isdst = -1;
        time_t t = xr_datetime_core_mktime(&tmv, dt->is_utc);
        xrt_datetime_object_t *result = xrt_datetime_alloc();
        result->timestamp = (int64_t) t;
        result->milliseconds = dt->milliseconds;
        result->tz_offset = dt->tz_offset;
        result->is_utc = dt->is_utc;
        return xrt_datetime_box(result);
    } else if (strcmp(unit, "year") == 0 || strcmp(unit, "years") == 0) {
        struct tm tmv;
        xrt_datetime_to_tm_obj(dt, &tmv);
        tmv.tm_year += (int) amount;
        int max_day = xr_datetime_core_days_in_month(tmv.tm_year + 1900, tmv.tm_mon + 1);
        if (tmv.tm_mday > max_day)
            tmv.tm_mday = max_day;
        tmv.tm_isdst = -1;
        time_t t = xr_datetime_core_mktime(&tmv, dt->is_utc);
        xrt_datetime_object_t *result = xrt_datetime_alloc();
        result->timestamp = (int64_t) t;
        result->milliseconds = dt->milliseconds;
        result->tz_offset = dt->tz_offset;
        result->is_utc = dt->is_utc;
        return xrt_datetime_box(result);
    } else {
        seconds = amount;
    }

    xrt_datetime_object_t *result = xrt_datetime_alloc();
    result->timestamp = dt->timestamp + seconds;
    result->milliseconds = dt->milliseconds;
    result->tz_offset = dt->tz_offset;
    result->is_utc = dt->is_utc;
    return xrt_datetime_box(result);
}

static inline XrValue xrt_datetime_diff_value(XrValue recv, XrValue other_v, XrValue unit_v) {
    const xrt_datetime_object_t *a = xrt_datetime_ptr(recv);
    const xrt_datetime_object_t *b = xrt_datetime_ptr(other_v);
    if (!a || !b)
        return XR_FROM_INT(0);
    const char *unit = XR_IS_STR(unit_v) ? xr_str_data(unit_v) : "seconds";
    int64_t diff_ms = (a->timestamp - b->timestamp) * 1000 + (a->milliseconds - b->milliseconds);
    if (strcmp(unit, "millisecond") == 0 || strcmp(unit, "milliseconds") == 0)
        return XR_FROM_INT(diff_ms);
    if (strcmp(unit, "second") == 0 || strcmp(unit, "seconds") == 0)
        return XR_FROM_INT(diff_ms / 1000);
    if (strcmp(unit, "minute") == 0 || strcmp(unit, "minutes") == 0)
        return XR_FROM_INT(diff_ms / 60000);
    if (strcmp(unit, "hour") == 0 || strcmp(unit, "hours") == 0)
        return XR_FROM_INT(diff_ms / 3600000);
    if (strcmp(unit, "day") == 0 || strcmp(unit, "days") == 0)
        return XR_FROM_INT(diff_ms / 86400000);
    if (strcmp(unit, "week") == 0 || strcmp(unit, "weeks") == 0)
        return XR_FROM_INT(diff_ms / 604800000);
    return XR_FROM_INT(diff_ms / 1000);
}

static inline XrValue xrt_datetime_to_utc_value(XrValue recv) {
    const xrt_datetime_object_t *dt = xrt_datetime_ptr(recv);
    if (!dt)
        return XR_NULL_VAL;
    xrt_datetime_object_t *result = xrt_datetime_alloc();
    result->timestamp = dt->timestamp;
    result->milliseconds = dt->milliseconds;
    result->tz_offset = 0;
    result->is_utc = 1;
    return xrt_datetime_box(result);
}

static inline XrValue xrt_datetime_to_local_value(XrValue recv) {
    const xrt_datetime_object_t *dt = xrt_datetime_ptr(recv);
    if (!dt)
        return XR_NULL_VAL;
    xrt_datetime_object_t *result = xrt_datetime_alloc();
    result->timestamp = dt->timestamp;
    result->milliseconds = dt->milliseconds;
    result->tz_offset =
        dt->is_utc ? xr_datetime_core_local_offset_at((time_t) dt->timestamp) : dt->tz_offset;
    result->is_utc = 0;
    return xrt_datetime_box(result);
}

static inline XrValue xrt_datetime_getprop(XrValue recv, int sym) {
    const xrt_datetime_object_t *dt = xrt_datetime_ptr(recv);
    if (!dt)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_YEAR)
        return XR_FROM_INT(xrt_datetime_year_ptr(dt));
    if (sym == XRT_SYM_MONTH)
        return XR_FROM_INT(xrt_datetime_month_ptr(dt));
    if (sym == XRT_SYM_DAY)
        return XR_FROM_INT(xrt_datetime_day_ptr(dt));
    if (sym == XRT_SYM_HOUR)
        return XR_FROM_INT(xrt_datetime_hour_ptr(dt));
    if (sym == XRT_SYM_MINUTE)
        return XR_FROM_INT(xrt_datetime_minute_ptr(dt));
    if (sym == XRT_SYM_SECOND)
        return XR_FROM_INT(xrt_datetime_second_ptr(dt));
    if (sym == XRT_SYM_MILLISECOND)
        return XR_FROM_INT(dt->milliseconds);
    if (sym == XRT_SYM_WEEKDAY)
        return XR_FROM_INT(xrt_datetime_weekday_ptr(dt));
    if (sym == XRT_SYM_YEARDAY)
        return XR_FROM_INT(xrt_datetime_yearday_ptr(dt));
    if (sym == XRT_SYM_TIMESTAMP)
        return XR_FROM_INT(dt->timestamp);
    return XR_NULL_VAL;
}

static inline XrValue xrt_datetime_method_0(XrValue recv, int sym) {
    const xrt_datetime_object_t *dt = xrt_datetime_ptr(recv);
    if (!dt)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_TOSTRING || sym == XRT_SYM_FORMAT)
        return xrt_datetime_to_string_value(dt);
    if (sym == XRT_SYM_TO_ISO_STRING)
        return xrt_datetime_to_iso_value(dt);
    if (sym == XRT_SYM_TO_UTC)
        return xrt_datetime_to_utc_value(recv);
    if (sym == XRT_SYM_TO_LOCAL)
        return xrt_datetime_to_local_value(recv);
    if (sym == XRT_SYM_IS_LEAP_YEAR)
        return XR_FROM_BOOL(xr_datetime_core_is_leap_year(xrt_datetime_year_ptr(dt)));
    if (sym == XRT_SYM_DAYS_IN_MONTH) {
        int y = xrt_datetime_year_ptr(dt);
        int m = xrt_datetime_month_ptr(dt);
        return XR_FROM_INT(xr_datetime_core_days_in_month(y, m));
    }
    return xrt_datetime_getprop(recv, sym);
}

static inline XrValue xrt_datetime_method_1(XrValue recv, int sym, XrValue arg0) {
    const xrt_datetime_object_t *dt = xrt_datetime_ptr(recv);
    if (!dt)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_FORMAT && XR_IS_STR(arg0))
        return xrt_datetime_format_pattern(dt, xr_str_data(arg0), xr_str_len(arg0));
    if (sym == XRT_SYM_IS_BEFORE)
        return XR_FROM_BOOL(xrt_datetime_is_before_ptr(dt, xrt_datetime_ptr(arg0)));
    if (sym == XRT_SYM_IS_AFTER)
        return XR_FROM_BOOL(xrt_datetime_is_after_ptr(dt, xrt_datetime_ptr(arg0)));
    if (sym == XRT_SYM_EQUALS)
        return XR_FROM_BOOL(xrt_datetime_equals_ptr(dt, xrt_datetime_ptr(arg0)));
    if (sym == XRT_SYM_DIFF)
        return xrt_datetime_diff_value(recv, arg0, XR_NULL_VAL);
    return XR_NULL_VAL;
}

static inline XrValue xrt_datetime_method_2(XrValue recv, int sym, XrValue arg0, XrValue arg1) {
    if (sym == XRT_SYM_ADD)
        return xrt_datetime_add_value(recv, arg0, arg1);
    if (sym == XRT_SYM_DIFF)
        return xrt_datetime_diff_value(recv, arg0, arg1);
    return XR_NULL_VAL;
}

#endif  // XRT_DATETIME_H
