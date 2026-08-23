/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi64_methods.h - i64 builtin method implementations.
 *
 * KEY POINTS:
 *   - sqrt() and pow() return f64 values.
 *   - max/min keep an i64 result for i64 operands and promote an f64 operand.
 *   - toString / toHex allocate a string; toBigInt allocates a BigInt.
 */

#ifndef XI64_METHODS_H
#define XI64_METHODS_H

#include "xvalue.h"
#include "../object/xstring.h"
#include "../object/xbigint.h"
#include "../symbol/xsymbol_table.h"
#include "../../coro/xcoroutine.h"
#include "../../shared/xr_int_arith_core.h"
#include "../../shared/xr_arith_core.h"

#include <math.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* i64.toString() -> decimal string. Allocates. */
static inline XrValue xr_i64_to_string_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                              int argc) {
    (void) args;
    (void) argc;
    XR_DCHECK(iso != NULL, "xr_i64_to_string_method: NULL isolate");
    char buffer[32];
    int len = snprintf(buffer, sizeof(buffer), "%lld", (long long) XR_TO_INT(self));
    XrString *str = xr_string_intern(iso, buffer, (size_t) len, 0);
    return xr_string_value(str);
}

/* i64.abs() -> i64. Pure, no GC.
 * INT64_MIN.abs() wraps to INT64_MIN: (-INT64_MIN) is signed-overflow UB,
 * so route the negate through unsigned to match wrap-on-overflow semantics
 * elsewhere in the language (see OP_UNM). */
static inline XrValue xr_i64_abs_method(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    xr_Integer v = XR_TO_INT(self);
    if (v >= 0)
        return xr_int(v);
    return xr_int(xr_i64_abs_wrap(v));
}

/* i64.toBigInt() -> BigInt. Allocates. */
static inline XrValue xr_i64_to_bigint_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                              int argc) {
    (void) args;
    (void) argc;
    XR_DCHECK(iso != NULL, "xr_i64_to_bigint_method: NULL isolate");
    XrBigInt *result = xr_bigint_new(NULL, XR_TO_INT(self));
    return XR_FROM_PTR(result);
}

/* i64.max(other) -> larger of self and other. Polymorphic on arg type. */
static inline XrValue xr_i64_max_method(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    xr_Integer v = XR_TO_INT(self);
    if (argc < 1)
        return xr_int(v);
    if (XR_IS_INT(args[0])) {
        xr_Integer other = XR_TO_INT(args[0]);
        return xr_int(v > other ? v : other);
    }
    if (XR_IS_FLOAT(args[0])) {
        xr_Number other = XR_TO_FLOAT(args[0]);
        xr_Number selfn = (xr_Number) v;
        return xr_float(selfn > other ? selfn : other);
    }
    return xr_int(v);
}

/* i64.min(other) -> smaller of self and other. Polymorphic on arg type. */
static inline XrValue xr_i64_min_method(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    xr_Integer v = XR_TO_INT(self);
    if (argc < 1)
        return xr_int(v);
    if (XR_IS_INT(args[0])) {
        xr_Integer other = XR_TO_INT(args[0]);
        return xr_int(v < other ? v : other);
    }
    if (XR_IS_FLOAT(args[0])) {
        xr_Number other = XR_TO_FLOAT(args[0]);
        xr_Number selfn = (xr_Number) v;
        return xr_float(selfn < other ? selfn : other);
    }
    return xr_int(v);
}

/* i64.toF64() -> float. Pure, no GC. */
static inline XrValue xr_i64_to_float_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                             int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_float((xr_Number) XR_TO_INT(self));
}

/* i64.toHex() -> hex string. Allocates. */
static inline XrValue xr_i64_to_hex_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                           int argc) {
    (void) args;
    (void) argc;
    XR_DCHECK(iso != NULL, "xr_i64_to_hex_method: NULL isolate");
    xr_Integer v = XR_TO_INT(self);
    char buffer[32];
    int len;
    if (v < 0) {
        len = snprintf(buffer, sizeof(buffer), "-0x%llX",
                       (unsigned long long) xr_i64_abs_magnitude(v));
    } else {
        len = snprintf(buffer, sizeof(buffer), "0x%llX", (unsigned long long) v);
    }
    XrString *str = xr_string_intern(iso, buffer, (size_t) len, 0);
    return xr_string_value(str);
}

/* i64.pow(exponent) -> float. Pure, no GC. */
static inline XrValue xr_i64_sqrt_method(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    xr_Number value = (xr_Number) XR_TO_INT(self);
    if (value < 0)
        return xr_float(NAN);
    return xr_float(sqrt(value));
}

static inline XrValue xr_i64_pow_method(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    xr_Number value = (xr_Number) XR_TO_INT(self);
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

static inline XrValue xr_i64_checked_add_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                                int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return XR_NULL_VAL;
    int64_t out;
    return xr_i64_checked_add(XR_TO_INT(self), XR_TO_INT(args[0]), &out) ? xr_int(out)
                                                                         : XR_NULL_VAL;
}

static inline XrValue xr_i64_checked_sub_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                                int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return XR_NULL_VAL;
    int64_t out;
    return xr_i64_checked_sub(XR_TO_INT(self), XR_TO_INT(args[0]), &out) ? xr_int(out)
                                                                         : XR_NULL_VAL;
}

static inline XrValue xr_i64_checked_mul_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                                int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return XR_NULL_VAL;
    int64_t out;
    return xr_i64_checked_mul(XR_TO_INT(self), XR_TO_INT(args[0]), &out) ? xr_int(out)
                                                                         : XR_NULL_VAL;
}

static inline XrValue xr_i64_saturating_add_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                                   int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return self;
    return xr_int(xr_i64_saturating_add(XR_TO_INT(self), XR_TO_INT(args[0])));
}

static inline XrValue xr_i64_saturating_sub_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                                   int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return self;
    return xr_int(xr_i64_saturating_sub(XR_TO_INT(self), XR_TO_INT(args[0])));
}

static inline XrValue xr_i64_saturating_mul_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                                   int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return self;
    return xr_int(xr_i64_saturating_mul(XR_TO_INT(self), XR_TO_INT(args[0])));
}

static inline XrValue xr_i64_wrapping_add_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                                 int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return self;
    return xr_int(xr_i64_add_wrap(XR_TO_INT(self), XR_TO_INT(args[0])));
}

static inline XrValue xr_i64_wrapping_sub_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                                 int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return self;
    return xr_int(xr_i64_sub_wrap(XR_TO_INT(self), XR_TO_INT(args[0])));
}

static inline XrValue xr_i64_wrapping_mul_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                                 int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return self;
    return xr_int(xr_i64_mul_wrap(XR_TO_INT(self), XR_TO_INT(args[0])));
}

/* --- Overflow predicates (task 153: moved from mem.*; semantic source
 * src/shared/xr_arith_core.h). Complement checkedAdd/...: the checked
 * family returns the value (or null), these only report the flag. --- */

/* i64.addOverflows(other) -> whether signed self + other overflows. */
static inline XrValue xr_i64_add_overflows_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                                  int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_bool(false);
    return xr_bool(xr_arith_core_add_overflows(XR_TO_INT(self), XR_TO_INT(args[0])) != 0);
}

/* i64.subOverflows(other) -> whether signed self - other overflows. */
static inline XrValue xr_i64_sub_overflows_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                                  int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_bool(false);
    return xr_bool(xr_arith_core_sub_overflows(XR_TO_INT(self), XR_TO_INT(args[0])) != 0);
}

/* i64.mulOverflows(other) -> whether signed self * other overflows. */
static inline XrValue xr_i64_mul_overflows_method(XrVMRuntime *iso, XrValue self, XrValue *args,
                                                  int argc) {
    (void) iso;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_bool(false);
    return xr_bool(xr_arith_core_mul_overflows(XR_TO_INT(self), XR_TO_INT(args[0])) != 0);
}

struct XrVMRuntime;
XR_FUNC void xr_i64_register_native_type(struct XrVMRuntime *isolate);

#ifdef __cplusplus
}
#endif

#endif /* XI64_METHODS_H */
