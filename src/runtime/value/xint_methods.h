/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xint_methods.h - Int builtin method implementations.
 *
 * KEY POINTS:
 *   - Math methods (floor / ceil / round) are no-ops on integer
 *     receivers; they return self instead of paying for a float
 *     round-trip the way the legacy dispatcher did.
 *   - sqrt() and pow() promote to float exactly like the legacy
 *     code path, preserving observable return-type behaviour.
 *   - max/min are polymorphic on the argument type: int+int -> int,
 *     int+float -> float (both branches preserve original semantics).
 *   - toString / toHex allocate a string; toBigInt allocates a BigInt.
 */

#ifndef XINT_METHODS_H
#define XINT_METHODS_H

#include "xvalue.h"
#include "../object/xstring.h"
#include "../object/xbigint.h"
#include "../symbol/xsymbol_table.h"
#include "../../coro/xcoroutine.h"

#include <math.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* int.toString() -> decimal string. Allocates. */
static inline XrValue xr_int_to_string_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                              int argc) {
    (void) args;
    (void) argc;
    XR_DCHECK(iso != NULL, "xr_int_to_string_method: NULL isolate");
    char buffer[32];
    int len = snprintf(buffer, sizeof(buffer), "%lld", (long long) XR_TO_INT(self));
    XrString *str = xr_string_intern(iso, buffer, (size_t) len, 0);
    return xr_string_value(str);
}

/* int.abs() -> int. Pure, no GC. */
static inline XrValue xr_int_abs_method(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    xr_Integer v = XR_TO_INT(self);
    return xr_int(v < 0 ? -v : v);
}

/* int.toBigInt() -> BigInt. Allocates. */
static inline XrValue xr_int_to_bigint_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                              int argc) {
    (void) args;
    (void) argc;
    XR_DCHECK(iso != NULL, "xr_int_to_bigint_method: NULL isolate");
    XrBigInt *result = xr_bigint_new(xr_current_coro(iso), XR_TO_INT(self));
    return XR_FROM_PTR(result);
}

/* int.max(other) -> larger of self and other. Polymorphic on arg type. */
static inline XrValue xr_int_max_method(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
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

/* int.min(other) -> smaller of self and other. Polymorphic on arg type. */
static inline XrValue xr_int_min_method(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
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

/* int.toFloat() -> float. Pure, no GC. */
static inline XrValue xr_int_to_float_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                             int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_float((xr_Number) XR_TO_INT(self));
}

/* int.toHex() -> hex string. Allocates. */
static inline XrValue xr_int_to_hex_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                           int argc) {
    (void) args;
    (void) argc;
    XR_DCHECK(iso != NULL, "xr_int_to_hex_method: NULL isolate");
    xr_Integer v = XR_TO_INT(self);
    char buffer[32];
    int len;
    if (v < 0) {
        len = snprintf(buffer, sizeof(buffer), "-0x%llX", (unsigned long long) (-v));
    } else {
        len = snprintf(buffer, sizeof(buffer), "0x%llX", (unsigned long long) v);
    }
    XrString *str = xr_string_intern(iso, buffer, (size_t) len, 0);
    return xr_string_value(str);
}

/* int.floor() -> int. No-op for an integer receiver. Pure, no GC. */
static inline XrValue xr_int_floor_method(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return self;
}

/* int.ceil() -> int. No-op for an integer receiver. Pure, no GC. */
static inline XrValue xr_int_ceil_method(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return self;
}

/* int.round() -> int. No-op for an integer receiver. Pure, no GC. */
static inline XrValue xr_int_round_method(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return self;
}

/* int.sqrt() -> float. NaN for negative input. Pure, no GC. */
static inline XrValue xr_int_sqrt_method(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    xr_Integer v = XR_TO_INT(self);
    if (v < 0)
        return xr_float(NAN);
    return xr_float(sqrt((xr_Number) v));
}

/* int.pow(exponent) -> float. Pure, no GC. */
static inline XrValue xr_int_pow_method(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
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

struct XrayIsolate;
XR_FUNC void xr_int_register_native_type(struct XrayIsolate *isolate);

#ifdef __cplusplus
}
#endif

#endif /* XINT_METHODS_H */
