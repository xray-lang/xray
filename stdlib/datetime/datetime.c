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
#include "../../src/base/xplatform.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/mem/xheap.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/class/xclass_builder.h"
#include "../../src/runtime/class/xclass_system.h"
#include "../../src/runtime/class/xinstance.h"
#include "../../src/base/xchecks.h"
#include "../../src/shared/xr_datetime_core.h"
#include "../../src/os/os_time.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#define XR_INT(n) XR_FROM_INT(n)

/* ========== Internal Helpers ========== */

/* Cached body offset for DateTime class instances (set in
 * xr_register_datetime_class). Since the class has 0 fields and a
 * fixed body alignment, this is a constant after registration and lets
 * xr_datetime_value() recover the instance pointer from a body pointer
 * via simple pointer arithmetic. */
static size_t g_datetime_body_offset = 0;

static XrClass *datetime_class(XrVMRuntime *X) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XR_DCHECK(core != NULL && core->dateTimeClass != NULL,
              "datetime: core->dateTimeClass not registered");
    return core->dateTimeClass;
}

static XrDateTime *datetime_alloc(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "datetime_alloc: isolate must not be NULL");
    XrInstance *inst = xr_instance_new(isolate, datetime_class(isolate));
    if (!inst)
        return NULL;
    XrDateTime *dt = (XrDateTime *) xr_instance_native_body(inst);
    XR_DCHECK(dt != NULL, "datetime_alloc: native body NULL");
    dt->timestamp = 0;
    dt->milliseconds = 0;
    dt->tz_offset = 0;
    dt->is_utc = 0;
    dt->_pad = 0;
    return dt;
}

/* ========== XrValue Conversion ========== */

bool xr_value_is_datetime(XrVMRuntime *X, XrValue v) {
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return xr_class_instanceof(inst->klass, datetime_class(X));
}

XrDateTime *xr_value_get_datetime_body(XrVMRuntime *X, XrValue v) {
    if (!xr_value_is_datetime(X, v))
        return NULL;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return (XrDateTime *) xr_instance_native_body(inst);
}

XrValue xr_datetime_value(XrDateTime *body) {
    if (!body)
        return xr_null();
    XR_DCHECK(g_datetime_body_offset > 0, "xr_datetime_value: class not registered yet");
    XrInstance *inst = (XrInstance *) ((uint8_t *) body - g_datetime_body_offset);
    return XR_FROM_PTR(inst);
}

static int64_t get_current_millis(void) {
    return (int64_t) (xr_time_realtime_ns() / 1000000ULL);
}

int xr_datetime_local_offset(void) {
    return xr_datetime_core_local_offset_at(time(NULL));
}

/* ========== Creation API ========== */

XrDateTime *xr_datetime_now(XrVMRuntime *isolate) {
    XrDateTime *dt = datetime_alloc(isolate);
    int64_t millis = get_current_millis();
    dt->timestamp = millis / 1000;
    dt->milliseconds = millis % 1000;
    dt->tz_offset = xr_datetime_local_offset();
    dt->is_utc = 0;
    return dt;
}

XrDateTime *xr_datetime_utc(XrVMRuntime *isolate) {
    XrDateTime *dt = datetime_alloc(isolate);
    int64_t millis = get_current_millis();
    dt->timestamp = millis / 1000;
    dt->milliseconds = millis % 1000;
    dt->tz_offset = 0;
    dt->is_utc = 1;
    return dt;
}

XrDateTime *xr_datetime_create(XrVMRuntime *isolate, int year, int month, int day, int hour,
                               int minute, int second, int is_utc) {
    struct tm tm = {0};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;

    time_t t = xr_datetime_core_mktime(&tm, is_utc);
    XrDateTime *dt = datetime_alloc(isolate);
    dt->timestamp = (int64_t) t;
    dt->milliseconds = 0;
    dt->tz_offset = is_utc ? 0 : xr_datetime_core_local_offset_at(t);
    dt->is_utc = (uint8_t) is_utc;
    return dt;
}

XrDateTime *xr_datetime_from_timestamp(XrVMRuntime *isolate, int64_t timestamp) {
    XrDateTime *dt = datetime_alloc(isolate);
    dt->timestamp = timestamp;
    dt->milliseconds = 0;
    dt->tz_offset = 0;
    dt->is_utc = 1;
    return dt;
}

XrDateTime *xr_datetime_from_timestamp_ms(XrVMRuntime *isolate, int64_t timestamp_ms) {
    XrDateTime *dt = datetime_alloc(isolate);
    // Floor division: milliseconds must be in [0, 999].
    // C truncation toward zero gives negative remainder for negative inputs.
    int64_t sec = timestamp_ms / 1000;
    int32_t ms = (int32_t) (timestamp_ms % 1000);
    if (ms < 0) {
        sec -= 1;
        ms += 1000;
    }
    dt->timestamp = sec;
    dt->milliseconds = ms;
    dt->tz_offset = 0;
    dt->is_utc = 1;
    return dt;
}

/* ========== Parse API ========== */

XrDateTime *xr_datetime_parse(XrVMRuntime *isolate, const char *str, const char *format) {
    XrDateTimeCoreFields fields;
    if (!xr_datetime_core_parse_fields(str, format, &fields))
        return NULL;

    XrDateTime *dt = datetime_alloc(isolate);
    if (!dt)
        return NULL;
    dt->timestamp = fields.timestamp;
    dt->milliseconds = fields.milliseconds;
    dt->tz_offset = fields.tz_offset;
    dt->is_utc = fields.is_utc;
    return dt;
}

/* ========== Format API ========== */

static int datetime_ctxbuf_write(void *ctx, const char *data, size_t len) {
    xr_ctxbuf_append((XrCtxBuf *) ctx, data, len);
    return 0;
}

static int datetime_copy_ctxbuf_to_cstr(const XrCtxBuf *out, char *buf, size_t buf_size) {
    if (!out || !buf || buf_size == 0)
        return 0;
    size_t len = out->len < buf_size - 1 ? out->len : buf_size - 1;
    if (out->data)
        memcpy(buf, out->data, len);
    buf[len] = '\0';
    return (int) len;
}

void xr_datetime_to_tm(XrDateTime *dt, struct tm *tm) {
    XR_DCHECK(dt != NULL && tm != NULL, "xr_datetime_to_tm: args must not be NULL");
    XrDateTimeCoreFields fields = {
        .timestamp = dt->timestamp,
        .milliseconds = dt->milliseconds,
        .tz_offset = dt->tz_offset,
        .is_utc = dt->is_utc,
    };
    xr_datetime_core_to_tm_fields(&fields, tm);
}

int xr_datetime_format(XrDateTime *dt, const char *pattern, char *buf, size_t buf_size) {
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);

    // Build into an XrCtxBuf so the full formatted output is produced even
    // for very long patterns. We then memcpy the prefix that fits into the
    // caller-provided fixed-size slot, keeping the legacy API contract.
    XrCtxBuf out;
    xr_ctxbuf_init(&out, 64);
    XrDateTimeCoreWriter writer = {.ctx = &out, .write = datetime_ctxbuf_write};
    (void) xr_datetime_core_format_tm(&writer, &tm, dt->milliseconds, pattern, -1);
    int len = datetime_copy_ctxbuf_to_cstr(&out, buf, buf_size);
    xr_ctxbuf_free(&out);
    return len;
}

int xr_datetime_to_iso_string(XrDateTime *dt, char *buf, size_t buf_size) {
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);
    XrCtxBuf out;
    xr_ctxbuf_init(&out, 64);
    XrDateTimeCoreWriter writer = {.ctx = &out, .write = datetime_ctxbuf_write};
    (void) xr_datetime_core_iso_write(&writer, &tm, dt->milliseconds, dt->is_utc, dt->tz_offset);
    int len = datetime_copy_ctxbuf_to_cstr(&out, buf, buf_size);
    xr_ctxbuf_free(&out);
    return len;
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
    return xr_datetime_core_is_leap_year(xr_datetime_year(dt));
}

int xr_datetime_days_in_month(XrDateTime *dt) {
    return xr_datetime_core_days_in_month(xr_datetime_year(dt), xr_datetime_month(dt));
}

/* ========== Date Arithmetic API ========== */

XrDateTime *xr_datetime_add(XrVMRuntime *isolate, XrDateTime *dt, int64_t amount,
                            const char *unit) {
    XrDateTimeCoreFields input = {
        .timestamp = dt->timestamp,
        .milliseconds = dt->milliseconds,
        .tz_offset = dt->tz_offset,
        .is_utc = dt->is_utc,
    };
    XrDateTimeCoreFields output;
    if (!xr_datetime_core_add_fields(&input, amount, unit, &output)) {
        fprintf(stderr, "datetime.add(): unknown unit '%s'\n", unit);
    }

    XrDateTime *result = datetime_alloc(isolate);
    result->timestamp = output.timestamp;
    result->milliseconds = output.milliseconds;
    result->tz_offset = output.tz_offset;
    result->is_utc = output.is_utc;
    return result;
}

int64_t xr_datetime_diff(XrDateTime *dt1, XrDateTime *dt2, const char *unit) {
    XrDateTimeCoreFields a = {
        .timestamp = dt1->timestamp,
        .milliseconds = dt1->milliseconds,
        .tz_offset = dt1->tz_offset,
        .is_utc = dt1->is_utc,
    };
    XrDateTimeCoreFields b = {
        .timestamp = dt2->timestamp,
        .milliseconds = dt2->milliseconds,
        .tz_offset = dt2->tz_offset,
        .is_utc = dt2->is_utc,
    };
    return xr_datetime_core_diff_fields(&a, &b, unit);
}

/* ========== Timezone API ========== */

// Copy every user-visible field explicitly instead of doing a raw memcpy
// past the object header: the old byte-level copy silently broke every time
// XrObjHeader changed layout, and produced subtle aliasing issues when header
// metadata grew.
static void datetime_copy_fields(XrDateTime *dst, const XrDateTime *src) {
    dst->timestamp = src->timestamp;
    dst->milliseconds = src->milliseconds;
    dst->tz_offset = src->tz_offset;
    dst->is_utc = src->is_utc;
}

XrDateTime *xr_datetime_to_utc(XrVMRuntime *isolate, XrDateTime *dt) {
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

XrDateTime *xr_datetime_to_local(XrVMRuntime *isolate, XrDateTime *dt) {
    XrDateTime *result = datetime_alloc(isolate);
    if (!result)
        return NULL;
    if (!dt->is_utc) {
        datetime_copy_fields(result, dt);
    } else {
        result->timestamp = dt->timestamp;
        result->milliseconds = dt->milliseconds;
        result->tz_offset = xr_datetime_core_local_offset_at((time_t) dt->timestamp);
        result->is_utc = 0;
    }
    return result;
}

/* ========== Module Binding Functions ========== */

// Module-level functions use XrCFunctionPtr (iso, args, argc) for XRS_EXPORT.
// Instance methods use XrPrimitiveMethodFn (iso, self, args, argc) for native type table.

static XrValue dt_now(XrVMRuntime *isolate, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    return xr_datetime_value(xr_datetime_now(isolate));
}

static XrValue dt_utc(XrVMRuntime *isolate, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    return xr_datetime_value(xr_datetime_utc(isolate));
}

static XrValue dt_create(XrVMRuntime *isolate, XrValue *args, int nargs) {
    int year = nargs > 0 && XR_IS_INT(args[0]) ? (int) XR_TO_INT(args[0]) : 1970;
    int month = nargs > 1 && XR_IS_INT(args[1]) ? (int) XR_TO_INT(args[1]) : 1;
    int day = nargs > 2 && XR_IS_INT(args[2]) ? (int) XR_TO_INT(args[2]) : 1;
    int hour = nargs > 3 && XR_IS_INT(args[3]) ? (int) XR_TO_INT(args[3]) : 0;
    int minute = nargs > 4 && XR_IS_INT(args[4]) ? (int) XR_TO_INT(args[4]) : 0;
    int second = nargs > 5 && XR_IS_INT(args[5]) ? (int) XR_TO_INT(args[5]) : 0;
    return xr_datetime_value(
        xr_datetime_create(isolate, year, month, day, hour, minute, second, 0));
}

static XrValue dt_create_utc(XrVMRuntime *isolate, XrValue *args, int nargs) {
    int year = nargs > 0 && XR_IS_INT(args[0]) ? (int) XR_TO_INT(args[0]) : 1970;
    int month = nargs > 1 && XR_IS_INT(args[1]) ? (int) XR_TO_INT(args[1]) : 1;
    int day = nargs > 2 && XR_IS_INT(args[2]) ? (int) XR_TO_INT(args[2]) : 1;
    int hour = nargs > 3 && XR_IS_INT(args[3]) ? (int) XR_TO_INT(args[3]) : 0;
    int minute = nargs > 4 && XR_IS_INT(args[4]) ? (int) XR_TO_INT(args[4]) : 0;
    int second = nargs > 5 && XR_IS_INT(args[5]) ? (int) XR_TO_INT(args[5]) : 0;
    return xr_datetime_value(
        xr_datetime_create(isolate, year, month, day, hour, minute, second, 1));
}

static XrValue dt_from_timestamp(XrVMRuntime *isolate, XrValue *args, int nargs) {
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

static XrValue dt_from_timestamp_ms(XrVMRuntime *isolate, XrValue *args, int nargs) {
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

static XrValue dt_parse(XrVMRuntime *isolate, XrValue *args, int nargs) {
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

static XrValue dt_offset(XrVMRuntime *isolate, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return XR_INT(xr_datetime_local_offset());
}

// Method binding: self = DateTime instance

static XrValue dt_to_string(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_NULL_VAL;
    XrDateTime *dt = xr_value_get_datetime_body(isolate, self);
    char buf[64];
    int n = xr_datetime_format(dt, "YYYY-MM-DD HH:mm:ss", buf, sizeof(buf));
    if (n <= 0)
        return XR_NULL_VAL;
    return xr_string_value(xr_string_new(isolate, buf, n));
}

static XrValue dt_format(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    if (!xr_value_is_datetime(isolate, self))
        return XR_NULL_VAL;
    XrDateTime *dt = xr_value_get_datetime_body(isolate, self);
    const char *pattern = "YYYY-MM-DD HH:mm:ss";
    if (nargs > 0 && XR_IS_STRING(args[0])) {
        pattern = XR_STRING_CHARS(XR_TO_STRING(args[0]));
    }

    // Build into a dynamic buffer so long custom patterns (e.g. embedding
    // localized strings) are never silently truncated.
    XrCtxBuf out;
    xr_ctxbuf_init(&out, 64);
    struct tm tm;
    xr_datetime_to_tm(dt, &tm);
    XrDateTimeCoreWriter writer = {.ctx = &out, .write = datetime_ctxbuf_write};
    (void) xr_datetime_core_format_tm(&writer, &tm, dt->milliseconds, pattern, -1);
    XrValue v = xr_string_value(xr_string_new(isolate, out.data ? out.data : "", out.len));
    xr_ctxbuf_free(&out);
    return v;
}

static XrValue dt_to_iso(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_NULL_VAL;
    char buf[64];
    int len =
        xr_datetime_to_iso_string(xr_value_get_datetime_body(isolate, self), buf, sizeof(buf));
    return xr_string_value(xr_string_new(isolate, buf, len));
}

static XrValue dt_year(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_INT(0);
    return XR_INT(xr_datetime_year(xr_value_get_datetime_body(isolate, self)));
}

static XrValue dt_month(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_INT(0);
    return XR_INT(xr_datetime_month(xr_value_get_datetime_body(isolate, self)));
}

static XrValue dt_day(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_INT(0);
    return XR_INT(xr_datetime_day(xr_value_get_datetime_body(isolate, self)));
}

static XrValue dt_hour(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_INT(0);
    return XR_INT(xr_datetime_hour(xr_value_get_datetime_body(isolate, self)));
}

static XrValue dt_minute(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_INT(0);
    return XR_INT(xr_datetime_minute(xr_value_get_datetime_body(isolate, self)));
}

static XrValue dt_second(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_INT(0);
    return XR_INT(xr_datetime_second(xr_value_get_datetime_body(isolate, self)));
}

static XrValue dt_millisecond(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_INT(0);
    return XR_INT(xr_datetime_millisecond(xr_value_get_datetime_body(isolate, self)));
}

static XrValue dt_weekday(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_INT(0);
    return XR_INT(xr_datetime_weekday(xr_value_get_datetime_body(isolate, self)));
}

static XrValue dt_yearday(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_INT(0);
    return XR_INT(xr_datetime_yearday(xr_value_get_datetime_body(isolate, self)));
}

static XrValue dt_timestamp(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_INT(0);
    return XR_INT(xr_value_get_datetime_body(isolate, self)->timestamp);
}

static XrValue dt_add(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    if (!xr_value_is_datetime(isolate, self) || nargs < 2 || !XR_IS_STRING(args[1]))
        return XR_NULL_VAL;
    XrDateTime *dt = xr_value_get_datetime_body(isolate, self);
    int64_t amount = XR_IS_INT(args[0]) ? XR_TO_INT(args[0]) : 0;
    const char *unit = XR_STRING_CHARS(XR_TO_STRING(args[1]));
    return xr_datetime_value(xr_datetime_add(isolate, dt, amount, unit));
}

static XrValue dt_diff(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    if (!xr_value_is_datetime(isolate, self) || nargs < 1 ||
        !xr_value_is_datetime(isolate, args[0]))
        return XR_INT(0);
    XrDateTime *dt1 = xr_value_get_datetime_body(isolate, self);
    XrDateTime *dt2 = xr_value_get_datetime_body(isolate, args[0]);
    const char *unit = "seconds";
    if (nargs > 1 && XR_IS_STRING(args[1])) {
        unit = XR_STRING_CHARS(XR_TO_STRING(args[1]));
    }
    return XR_INT(xr_datetime_diff(dt1, dt2, unit));
}

static XrValue dt_to_utc(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_NULL_VAL;
    return xr_datetime_value(
        xr_datetime_to_utc(isolate, xr_value_get_datetime_body(isolate, self)));
}

static XrValue dt_to_local(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_NULL_VAL;
    return xr_datetime_value(
        xr_datetime_to_local(isolate, xr_value_get_datetime_body(isolate, self)));
}

static XrValue dt_is_before(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    if (!xr_value_is_datetime(isolate, self) || nargs < 1 ||
        !xr_value_is_datetime(isolate, args[0]))
        return XR_FALSE_VAL;
    return xr_datetime_is_before(xr_value_get_datetime_body(isolate, self),
                                 xr_value_get_datetime_body(isolate, args[0]))
               ? XR_TRUE_VAL
               : XR_FALSE_VAL;
}

static XrValue dt_is_after(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    if (!xr_value_is_datetime(isolate, self) || nargs < 1 ||
        !xr_value_is_datetime(isolate, args[0]))
        return XR_FALSE_VAL;
    return xr_datetime_is_after(xr_value_get_datetime_body(isolate, self),
                                xr_value_get_datetime_body(isolate, args[0]))
               ? XR_TRUE_VAL
               : XR_FALSE_VAL;
}

static XrValue dt_equals(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    if (!xr_value_is_datetime(isolate, self) || nargs < 1 ||
        !xr_value_is_datetime(isolate, args[0]))
        return XR_FALSE_VAL;
    return xr_datetime_equals(xr_value_get_datetime_body(isolate, self),
                              xr_value_get_datetime_body(isolate, args[0]))
               ? XR_TRUE_VAL
               : XR_FALSE_VAL;
}

static XrValue dt_is_leap_year(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_FALSE_VAL;
    return xr_datetime_is_leap_year(xr_value_get_datetime_body(isolate, self)) ? XR_TRUE_VAL
                                                                               : XR_FALSE_VAL;
}

static XrValue dt_days_in_month(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    if (!xr_value_is_datetime(isolate, self))
        return XR_INT(0);
    return XR_INT(xr_datetime_days_in_month(xr_value_get_datetime_body(isolate, self)));
}

#define XR_STDLIB_VM_BIND_MODULE_DATETIME 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_DATETIME

/* ========== Native Body Descriptor ========== */

static void datetime_body_init(XrInstance *inst, void *body) {
    (void) inst;
    XrDateTime *dt = (XrDateTime *) body;
    dt->timestamp = 0;
    dt->milliseconds = 0;
    dt->tz_offset = 0;
    dt->is_utc = 0;
    dt->_pad = 0;
}

/* deep_copy / to_shared: bytes copy via xr_instance_clone_layout in the
 * unified instance lifecycle (already handled in xinstance.c). NULL
 * means "rely on the default memcpy of fields[] + native body". */
static XrNativeBodyDesc g_datetime_body_desc = {
    .body_size = sizeof(XrDateTime),
    .body_align = _Alignof(int64_t),
    .copy_policy = XR_NATIVE_BODY_COPY_DEEP,
    .init = datetime_body_init,
    .destroy = NULL,
    .deep_copy = NULL,
    .to_shared = NULL,
};

#define XR_STDLIB_VM_BIND_CLASS_DATE_TIME 1
#include "../../src/stdlib/xstdlib_class_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_CLASS_DATE_TIME

/* DateTime class registration is invoked unconditionally during isolate
 * init by xr_prelude_register_all_native_types, so the XrClass is
 * available even when user code never `import datetime`. */
void xr_register_datetime_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_date_time_class_generated(isolate);
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL && core->dateTimeClass != NULL,
              "register_datetime_class: DateTime not registered");
    g_datetime_body_offset = xr_instance_body_offset(core->dateTimeClass);
}

XrModule *xr_load_module_datetime(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_datetime: NULL isolate");

    // Create module — only factory functions exported (the DateTime
    // XrClass itself is registered up front by the prelude module).
    XrModule *mod = xr_module_create_native(isolate, "datetime");
    if (!mod)
        return NULL;

    xr_stdlib_vm_bind_datetime_generated(isolate, mod);

    mod->loaded = true;
    return mod;
}
