/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xf64_methods.h - f64 builtin method implementations.
 *
 * KEY POINTS:
 *   - All f64 methods are `static inline` here so AOT-generated C
 *     inlines them at the call site. The address-take inside
 *     xr_f64_method_table[] forces a single out-of-line copy for
 *     the VM dispatcher.
 *   - Pure / no-GC predicates carry the matching flags so AOT
 *     specializers can hoist them above safepoints.
 */

#ifndef XF64_METHODS_H
#define XF64_METHODS_H

#include "xvalue.h"
#include "../object/xstring.h"
#include "../symbol/xsymbol_table.h"
#include "../../base/xconstants.h"
#include "../../shared/xr_float_fmt.h"
#include "../../shared/xr_numeric_core.h"

#include <math.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* f64.toString() -> shortest round-trip string. Allocates.
 * Guarantees a decimal point so 0.0.toString() == "0.0", not "0". */
static inline XrValue xr_f64_to_string_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                                int argc) {
    (void) args;
    (void) argc;
    XR_DCHECK(iso != NULL, "xr_f64_to_string_method: NULL isolate");
    char buffer[64];
    int len = xr_format_float(buffer, sizeof(buffer), XR_TO_FLOAT(self));
    XrString *str = xr_string_intern(iso, buffer, (size_t) len, 0);
    return xr_string_value(str);
}

/* f64.toFixed(decimals=0) -> fixed-precision string. Allocates. */
static inline XrValue xr_f64_to_fixed_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                               int argc) {
    XR_DCHECK(iso != NULL, "xr_f64_to_fixed_method: NULL isolate");
    int64_t decimals = (argc >= 1 && XR_IS_INT(args[0])) ? XR_TO_INT(args[0]) : 0;
    char buffer[64];
    int len = xr_numeric_core_format_fixed(buffer, sizeof(buffer), XR_TO_FLOAT(self), decimals);
    XrString *str = xr_string_intern(iso, buffer, (size_t) len, 0);
    return xr_string_value(str);
}

/* f64.floor() -> int. Pure, no GC. */
static inline XrValue xr_f64_floor_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                            int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_int((xr_Integer) floor(XR_TO_FLOAT(self)));
}

/* f64.ceil() -> int. Pure, no GC. */
static inline XrValue xr_f64_ceil_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                           int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_int((xr_Integer) ceil(XR_TO_FLOAT(self)));
}

/* f64.round() -> int. Pure, no GC. */
static inline XrValue xr_f64_round_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                            int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_int((xr_Integer) round(XR_TO_FLOAT(self)));
}

/* f64.abs() -> f64. Pure, no GC. */
static inline XrValue xr_f64_abs_method(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_float(fabs(XR_TO_FLOAT(self)));
}

/* f64.sqrt() -> f64. Returns NaN for negative input. Pure, no GC. */
static inline XrValue xr_f64_sqrt_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                           int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    xr_Number value = XR_TO_FLOAT(self);
    if (value < 0)
        return xr_float(NAN);
    return xr_float(sqrt(value));
}

/* f64.isNaN() -> bool. Pure, no GC. */
static inline XrValue xr_f64_is_nan_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                             int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(isnan(XR_TO_FLOAT(self)));
}

/* f64.toI64() -> int (truncation). Pure, no GC. */
static inline XrValue xr_f64_to_int_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                             int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_int((xr_Integer) XR_TO_FLOAT(self));
}

/* f64.pow(exponent) -> f64. Pure, no GC. */
static inline XrValue xr_f64_pow_method(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
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

struct XrVMRuntime;
XR_FUNC void xr_f64_register_native_type(struct XrVMRuntime *isolate);

#ifdef __cplusplus
}
#endif

#endif /* XF64_METHODS_H */
