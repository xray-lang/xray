/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfloat_methods.h - Float builtin method implementations.
 *
 * KEY POINTS:
 *   - All float methods are `static inline` here so AOT-generated C
 *     inlines them at the call site. The address-take inside
 *     xr_float_method_table[] forces a single out-of-line copy for
 *     the VM dispatcher.
 *   - Pure / no-GC predicates carry the matching flags so JIT and AOT
 *     specializers can hoist them above safepoints.
 */

#ifndef XFLOAT_METHODS_H
#define XFLOAT_METHODS_H

#include "xmethod_table.h"
#include "xvalue.h"
#include "../object/xstring.h"
#include "../symbol/xsymbol_table.h"
#include "../../base/xconstants.h"

#include <math.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* float.toString() -> shortest round-trip string. Allocates. */
static inline XrValue xr_float_to_string_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                                int argc) {
    (void) args;
    (void) argc;
    XR_DCHECK(iso != NULL, "xr_float_to_string_method: NULL isolate");
    char buffer[64];
    int len = snprintf(buffer, sizeof(buffer), "%g", XR_TO_FLOAT(self));
    XrString *str = xr_string_intern(iso, buffer, (size_t) len, 0);
    return xr_string_value(str);
}

/* float.toFixed(decimals=0) -> fixed-precision string. Allocates. */
static inline XrValue xr_float_to_fixed_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                               int argc) {
    XR_DCHECK(iso != NULL, "xr_float_to_fixed_method: NULL isolate");
    int decimals = (argc >= 1 && XR_IS_INT(args[0])) ? (int) XR_TO_INT(args[0]) : 0;
    if (decimals < 0)
        decimals = 0;
    if (decimals > XR_TOFIXED_MAX_DECIMALS)
        decimals = XR_TOFIXED_MAX_DECIMALS;
    char buffer[64];
    int len = snprintf(buffer, sizeof(buffer), "%.*f", decimals, XR_TO_FLOAT(self));
    XrString *str = xr_string_intern(iso, buffer, (size_t) len, 0);
    return xr_string_value(str);
}

/* float.floor() -> int. Pure, no GC. */
static inline XrValue xr_float_floor_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                            int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_int((xr_Integer) floor(XR_TO_FLOAT(self)));
}

/* float.ceil() -> int. Pure, no GC. */
static inline XrValue xr_float_ceil_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                           int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_int((xr_Integer) ceil(XR_TO_FLOAT(self)));
}

/* float.round() -> int. Pure, no GC. */
static inline XrValue xr_float_round_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                            int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_int((xr_Integer) round(XR_TO_FLOAT(self)));
}

/* float.abs() -> float. Pure, no GC. */
static inline XrValue xr_float_abs_method(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_float(fabs(XR_TO_FLOAT(self)));
}

/* float.sqrt() -> float. Returns NaN for negative input. Pure, no GC. */
static inline XrValue xr_float_sqrt_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                           int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    xr_Number value = XR_TO_FLOAT(self);
    if (value < 0)
        return xr_float(NAN);
    return xr_float(sqrt(value));
}

/* float.toInt() -> int (truncation). Pure, no GC. */
static inline XrValue xr_float_to_int_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                             int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_int((xr_Integer) XR_TO_FLOAT(self));
}

/* float.pow(exponent) -> float. Pure, no GC. */
static inline XrValue xr_float_pow_method(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    xr_Number value = XR_TO_FLOAT(self);
    if (argc < 1)
        return xr_float(value);
    xr_Number exponent;
    if (XR_IS_FLOAT(args[0])) {
        exponent = XR_TO_FLOAT(args[0]);
    } else if (XR_IS_INT(args[0])) {
        exponent = (xr_Number) XR_TO_INT(args[0]);
    } else {
        return xr_float(value);
    }
    return xr_float(pow(value, exponent));
}

extern const XrMethodSlot xr_float_method_table[SYMBOL_BUILTIN_COUNT];

#ifdef __cplusplus
}
#endif

#endif /* XFLOAT_METHODS_H */
