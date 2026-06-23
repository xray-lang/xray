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

static inline XrDateTimeCoreFields xrt_datetime_core_fields(const xrt_datetime_object_t *dt) {
    return (XrDateTimeCoreFields) {
        .timestamp = dt ? dt->timestamp : 0,
        .milliseconds = dt ? dt->milliseconds : 0,
        .tz_offset = dt ? dt->tz_offset : 0,
        .is_utc = dt ? dt->is_utc : 0,
    };
}

static inline XrValue xrt_datetime_from_core_fields(const XrDateTimeCoreFields *fields) {
    if (!fields)
        return XR_NULL_VAL;
    xrt_datetime_object_t *dt = xrt_datetime_alloc();
    dt->timestamp = fields->timestamp;
    dt->milliseconds = fields->milliseconds;
    dt->tz_offset = fields->tz_offset;
    dt->is_utc = (uint8_t) (fields->is_utc ? 1 : 0);
    return xrt_datetime_box(dt);
}

static inline void xrt_datetime_to_tm_obj(const xrt_datetime_object_t *dt, struct tm *tmv) {
    XrDateTimeCoreFields fields = xrt_datetime_core_fields(dt);
    xr_datetime_core_to_tm_fields(&fields, tmv);
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
    XrDateTimeCoreFields fields =
        xr_datetime_core_from_timestamp(xrt_datetime_i64_arg(timestamp, 0));
    return xrt_datetime_from_core_fields(&fields);
}

static inline XrValue xrt_datetime_from_timestamp_ms(XrValue timestamp_ms) {
    XrDateTimeCoreFields fields =
        xr_datetime_core_from_timestamp_ms(xrt_datetime_i64_arg(timestamp_ms, 0));
    return xrt_datetime_from_core_fields(&fields);
}

static inline XrValue xrt_datetime_offset(void) {
    return XR_FROM_INT((int64_t) xr_datetime_core_local_offset_at(time(NULL)));
}

static inline XrValue xrt_datetime_parse_impl(const char *str, const char *format) {
    XrDateTimeCoreFields fields;
    if (!xr_datetime_core_parse_fields(str, format, &fields))
        return XR_NULL_VAL;

    return xrt_datetime_from_core_fields(&fields);
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
    XrDateTimeCoreFields af = xrt_datetime_core_fields(a);
    XrDateTimeCoreFields bf = xrt_datetime_core_fields(b);
    return a && b && xr_datetime_core_compare_fields(&af, &bf) < 0;
}

static inline int xrt_datetime_is_after_ptr(const xrt_datetime_object_t *a,
                                            const xrt_datetime_object_t *b) {
    XrDateTimeCoreFields af = xrt_datetime_core_fields(a);
    XrDateTimeCoreFields bf = xrt_datetime_core_fields(b);
    return a && b && xr_datetime_core_compare_fields(&af, &bf) > 0;
}

static inline int xrt_datetime_equals_ptr(const xrt_datetime_object_t *a,
                                          const xrt_datetime_object_t *b) {
    XrDateTimeCoreFields af = xrt_datetime_core_fields(a);
    XrDateTimeCoreFields bf = xrt_datetime_core_fields(b);
    return a && b && xr_datetime_core_compare_fields(&af, &bf) == 0;
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
    XrDateTimeCoreFields input = xrt_datetime_core_fields(dt);
    XrDateTimeCoreFields output;
    xr_datetime_core_add_fields(&input, amount, unit, &output);
    return xrt_datetime_from_core_fields(&output);
}

static inline XrValue xrt_datetime_diff_value(XrValue recv, XrValue other_v, XrValue unit_v) {
    const xrt_datetime_object_t *a = xrt_datetime_ptr(recv);
    const xrt_datetime_object_t *b = xrt_datetime_ptr(other_v);
    if (!a || !b)
        return XR_FROM_INT(0);
    const char *unit = XR_IS_STR(unit_v) ? xr_str_data(unit_v) : "seconds";
    XrDateTimeCoreFields af = xrt_datetime_core_fields(a);
    XrDateTimeCoreFields bf = xrt_datetime_core_fields(b);
    return XR_FROM_INT(xr_datetime_core_diff_fields(&af, &bf, unit));
}

static inline XrValue xrt_datetime_to_utc_value(XrValue recv) {
    const xrt_datetime_object_t *dt = xrt_datetime_ptr(recv);
    if (!dt)
        return XR_NULL_VAL;
    XrDateTimeCoreFields input = xrt_datetime_core_fields(dt);
    XrDateTimeCoreFields output;
    if (!xr_datetime_core_to_utc_fields(&input, &output))
        return XR_NULL_VAL;
    return xrt_datetime_from_core_fields(&output);
}

static inline XrValue xrt_datetime_to_local_value(XrValue recv) {
    const xrt_datetime_object_t *dt = xrt_datetime_ptr(recv);
    if (!dt)
        return XR_NULL_VAL;
    XrDateTimeCoreFields input = xrt_datetime_core_fields(dt);
    XrDateTimeCoreFields output;
    if (!xr_datetime_core_to_local_fields(&input, &output))
        return XR_NULL_VAL;
    return xrt_datetime_from_core_fields(&output);
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
