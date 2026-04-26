/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * datetime_methods.c - DateTime instance method bodies + dispatch table.
 */

#include "datetime_methods.h"
#include "datetime.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/value/xvalue_format.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/base/xchecks.h"
#include "../../src/base/xconstants.h"

static inline XrDateTime *dt_self(XrValue self) {
    XR_DCHECK(XR_IS_DATETIME(self), "datetime method: receiver is not DateTime");
    return XR_TO_DATETIME(self);
}

/* Formatting helpers */
static XrValue m_format(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrDateTime *dt = dt_self(self);
    const char *pattern = XR_DATETIME_DEFAULT_FORMAT;
    if (argc > 0 && XR_IS_STRING(args[0])) {
        pattern = xr_value_to_string(iso, args[0])->data;
    }
    char buf[256];
    int len = xr_datetime_format(dt, pattern, buf, sizeof(buf));
    return xr_string_value(xr_string_intern(iso, buf, (size_t) len, 0));
}

static XrValue m_to_iso_string(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrDateTime *dt = dt_self(self);
    char buf[64];
    int len = xr_datetime_to_iso_string(dt, buf, sizeof(buf));
    return xr_string_value(xr_string_intern(iso, buf, (size_t) len, 0));
}

static XrValue m_to_string(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrDateTime *dt = dt_self(self);
    char buf[256];
    int len = xr_datetime_format(dt, XR_DATETIME_DEFAULT_FORMAT, buf, sizeof(buf));
    return xr_string_value(xr_string_intern(iso, buf, (size_t) len, 0));
}

/* Field accessors — pure / no-GC (return immediate ints). */
#define DT_INT_GETTER(name, body)                                                                  \
    static XrValue name(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {                 \
        (void) iso;                                                                                \
        (void) args;                                                                               \
        (void) argc;                                                                               \
        XrDateTime *dt = dt_self(self);                                                            \
        return xr_int(body);                                                                       \
    }

DT_INT_GETTER(m_year, xr_datetime_year(dt))
DT_INT_GETTER(m_month, xr_datetime_month(dt))
DT_INT_GETTER(m_day, xr_datetime_day(dt))
DT_INT_GETTER(m_hour, xr_datetime_hour(dt))
DT_INT_GETTER(m_minute, xr_datetime_minute(dt))
DT_INT_GETTER(m_second, xr_datetime_second(dt))
DT_INT_GETTER(m_weekday, xr_datetime_weekday(dt))
DT_INT_GETTER(m_timestamp, dt->timestamp)
DT_INT_GETTER(m_millisecond, xr_datetime_millisecond(dt))
DT_INT_GETTER(m_yearday, xr_datetime_yearday(dt))
DT_INT_GETTER(m_days_in_month, xr_datetime_days_in_month(dt))

#undef DT_INT_GETTER

/* Conversions */
static XrValue m_to_utc(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrDateTime *out = xr_datetime_to_utc(iso, dt_self(self));
    return out ? xr_datetime_value(out) : xr_null();
}

static XrValue m_to_local(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrDateTime *out = xr_datetime_to_local(iso, dt_self(self));
    return out ? xr_datetime_value(out) : xr_null();
}

/* Comparisons */
static XrValue m_is_before(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_DATETIME(args[0]))
        return XR_FALSE_VAL;
    return xr_datetime_is_before(dt_self(self), XR_TO_DATETIME(args[0])) ? XR_TRUE_VAL
                                                                         : XR_FALSE_VAL;
}

static XrValue m_is_after(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_DATETIME(args[0]))
        return XR_FALSE_VAL;
    return xr_datetime_is_after(dt_self(self), XR_TO_DATETIME(args[0])) ? XR_TRUE_VAL
                                                                        : XR_FALSE_VAL;
}

static XrValue m_equals(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_DATETIME(args[0]))
        return XR_FALSE_VAL;
    return xr_datetime_equals(dt_self(self), XR_TO_DATETIME(args[0])) ? XR_TRUE_VAL : XR_FALSE_VAL;
}

static XrValue m_is_leap_year(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_datetime_is_leap_year(dt_self(self)) ? XR_TRUE_VAL : XR_FALSE_VAL;
}

#define PURE_NO_GC (XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC)

const XrMethodSlot xr_datetime_method_table[SYMBOL_BUILTIN_COUNT] = {
    [SYMBOL_FORMAT] = {m_format, 0, 1, XR_METHOD_FLAG_MAY_THROW},
    [SYMBOL_YEAR] = {m_year, 0, 0, PURE_NO_GC},
    [SYMBOL_MONTH] = {m_month, 0, 0, PURE_NO_GC},
    [SYMBOL_DAY] = {m_day, 0, 0, PURE_NO_GC},
    [SYMBOL_HOUR] = {m_hour, 0, 0, PURE_NO_GC},
    [SYMBOL_MINUTE] = {m_minute, 0, 0, PURE_NO_GC},
    [SYMBOL_SECOND] = {m_second, 0, 0, PURE_NO_GC},
    [SYMBOL_WEEKDAY] = {m_weekday, 0, 0, PURE_NO_GC},
    [SYMBOL_TIMESTAMP] = {m_timestamp, 0, 0, PURE_NO_GC},
    [SYMBOL_MILLISECOND] = {m_millisecond, 0, 0, PURE_NO_GC},
    [SYMBOL_YEARDAY] = {m_yearday, 0, 0, PURE_NO_GC},
    [SYMBOL_DAYS_IN_MONTH] = {m_days_in_month, 0, 0, PURE_NO_GC},
    [SYMBOL_TO_UTC] = {m_to_utc, 0, 0, 0},
    [SYMBOL_TO_LOCAL] = {m_to_local, 0, 0, 0},
    [SYMBOL_IS_BEFORE] = {m_is_before, 1, 1, PURE_NO_GC},
    [SYMBOL_IS_AFTER] = {m_is_after, 1, 1, PURE_NO_GC},
    [SYMBOL_EQUALS] = {m_equals, 1, 1, PURE_NO_GC},
    [SYMBOL_IS_LEAP_YEAR] = {m_is_leap_year, 0, 0, PURE_NO_GC},
    [SYMBOL_TO_ISO_STRING] = {m_to_iso_string, 0, 0, XR_METHOD_FLAG_MAY_THROW},
    [SYMBOL_TOSTRING] = {m_to_string, 0, 0, XR_METHOD_FLAG_MAY_THROW},
};

#undef PURE_NO_GC
