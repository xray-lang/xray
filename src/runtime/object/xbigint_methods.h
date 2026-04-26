/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbigint_methods.h - BigInt builtin method implementations.
 *
 * KEY POINTS:
 *   - All eight bigint methods (toString, abs, sign, isZero,
 *     isNegative, isPositive, toInt, toFloat) are thin wrappers around
 *     the bigint primitives in xbigint.h. They live as `static inline`
 *     here so AOT-generated C inlines the wrapper, leaving only the
 *     actual primitive call. Taking each address inside the table
 *     forces a single out-of-line copy for the VM dispatcher.
 *   - Methods that allocate (toString, abs) are still inline wrappers;
 *     the heavy lifting (xr_bigint_to_string, xr_bigint_abs) is extern
 *     so the body remains small. This means the VM and AOT both pay
 *     exactly one extern call per such method, no extra dispatch
 *     hop.
 */

#ifndef XBIGINT_METHODS_H
#define XBIGINT_METHODS_H

#include "../value/xmethod_table.h"
#include "../value/xvalue.h"
#include "../symbol/xsymbol_table.h"
#include "xbigint.h"
#include "xstring.h"
#include "../../coro/xcoroutine.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline XrBigInt *xr_bigint_method_self(XrValue self) {
    XR_DCHECK(XR_IS_BIGINT(self), "bigint method: receiver is not a BigInt");
    return (XrBigInt *) XR_TO_PTR(self);
}

/* bigint.toString() -> decimal string. Allocates. */
static inline XrValue xr_bigint_to_string_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                                 int argc) {
    (void) args;
    (void) argc;
    XR_DCHECK(iso != NULL, "xr_bigint_to_string_method: NULL isolate");
    XrBigInt *bi = xr_bigint_method_self(self);
    char *str = xr_bigint_to_string(bi);
    if (!str)
        return xr_null();
    XrString *result = xr_string_intern(iso, str, strlen(str), 0);
    xr_free(str);
    return xr_string_value(result);
}

/* bigint.abs() -> bigint. Allocates a new BigInt. */
static inline XrValue xr_bigint_abs_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                           int argc) {
    (void) args;
    (void) argc;
    XR_DCHECK(iso != NULL, "xr_bigint_abs_method: NULL isolate");
    XrBigInt *result = xr_bigint_abs(xr_current_coro(iso), xr_bigint_method_self(self));
    return XR_FROM_PTR(result);
}

/* bigint.sign() -> -1 / 0 / 1. Pure, no GC. */
static inline XrValue xr_bigint_sign_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                            int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    XrBigInt *bi = xr_bigint_method_self(self);
    if (xr_bigint_is_zero(bi))
        return xr_int(0);
    return xr_int(bi->sign);
}

/* bigint.isZero() -> bool. Pure, no GC. */
static inline XrValue xr_bigint_is_zero_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                               int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(xr_bigint_is_zero(xr_bigint_method_self(self)));
}

/* bigint.isNegative() -> bool. Pure, no GC. */
static inline XrValue xr_bigint_is_negative_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                                   int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    XrBigInt *bi = xr_bigint_method_self(self);
    return xr_bool(bi->sign < 0 && !xr_bigint_is_zero(bi));
}

/* bigint.isPositive() -> bool. Pure, no GC. */
static inline XrValue xr_bigint_is_positive_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                                   int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    XrBigInt *bi = xr_bigint_method_self(self);
    return xr_bool(bi->sign > 0 && !xr_bigint_is_zero(bi));
}

/* bigint.toInt() -> int. Returns null on overflow. Pure, no GC. */
static inline XrValue xr_bigint_to_int_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                              int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    bool overflow = false;
    int64_t value = xr_bigint_to_int64(xr_bigint_method_self(self), &overflow);
    if (overflow)
        return xr_null();
    return xr_int(value);
}

/* bigint.toFloat() -> float. May lose precision. Pure, no GC. */
static inline XrValue xr_bigint_to_float_method(XrayIsolate *iso, XrValue self, XrValue *args,
                                                int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_float(xr_bigint_to_double(xr_bigint_method_self(self)));
}

extern const XrMethodSlot xr_bigint_method_table[SYMBOL_BUILTIN_COUNT];

#ifdef __cplusplus
}
#endif

#endif /* XBIGINT_METHODS_H */
