/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * math.c - `math` private native leaves.
 *
 * KEY CONCEPT:
 *   The module's semantic truth is stdlib/math/math.xr. This file implements
 *   only the private leaves that file forwards to: libm routines whose accuracy
 *   contract Xray arithmetic cannot restate, plus the system random source.
 *   Every leaf is declared over f64, so none of them inspects the runtime shape
 *   of its argument.
 */

#include "../common.h"
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include "../../src/os/os_random.h"
#include "../../src/shared/xr_math_core.h"

// Portability: MSVC/<math.h> does not define M_PI/M_E unless _USE_MATH_DEFINES
// is set at the translation-unit level. Provide the standard constants here
// so this file compiles cleanly on every supported toolchain.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif
#ifndef M_LN2
#define M_LN2 0.69314718055994530942
#endif
#ifndef M_LN10
#define M_LN10 2.30258509299404568402
#endif
#ifndef M_LOG2E
#define M_LOG2E 1.44269504088896340736
#endif
#ifndef M_LOG10E
#define M_LOG10E 0.43429448190325182765
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

// Safe range for double-to-int64 cast: [INT64_MIN, INT64_MAX]
#define DOUBLE_FITS_INT64(d) ((d) >= (double) INT64_MIN && (d) < (double) INT64_MAX + 1.0)

/* ========== Basic Math ========== */

static XrValue math_abs(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(fabs(XR_TO_FLOAT(args[0])));
}

static XrValue math_floor(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(0);
    double result = floor(XR_TO_FLOAT(args[0]));
    if (DOUBLE_FITS_INT64(result))
        return xr_int((int64_t) result);
    return xr_float(result);
}

static XrValue math_ceil(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(0);
    double result = ceil(XR_TO_FLOAT(args[0]));
    if (DOUBLE_FITS_INT64(result))
        return xr_int((int64_t) result);
    return xr_float(result);
}

static XrValue math_round(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(0);
    double result = round(XR_TO_FLOAT(args[0]));
    if (DOUBLE_FITS_INT64(result))
        return xr_int((int64_t) result);
    return xr_float(result);
}

static XrValue math_sqrt(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = XR_TO_FLOAT(args[0]);
    return xr_float(sqrt(v));
}

static XrValue math_pow(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_float(0.0);
    double x = XR_TO_FLOAT(args[0]);
    double y = XR_TO_FLOAT(args[1]);
    return xr_float(pow(x, y));
}

/* ========== Trigonometric ========== */

static XrValue math_sin(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = XR_TO_FLOAT(args[0]);
    return xr_float(sin(v));
}

static XrValue math_cos(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = XR_TO_FLOAT(args[0]);
    return xr_float(cos(v));
}

static XrValue math_tan(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = XR_TO_FLOAT(args[0]);
    return xr_float(tan(v));
}

static XrValue math_asin(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = XR_TO_FLOAT(args[0]);
    return xr_float(asin(v));
}

static XrValue math_acos(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = XR_TO_FLOAT(args[0]);
    return xr_float(acos(v));
}

static XrValue math_atan(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = XR_TO_FLOAT(args[0]);
    return xr_float(atan(v));
}

static XrValue math_atan2(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_float(0.0);
    double y = XR_TO_FLOAT(args[0]);
    double x = XR_TO_FLOAT(args[1]);
    return xr_float(atan2(y, x));
}

/* ========== Logarithmic & Exponential ========== */

static XrValue math_log(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = XR_TO_FLOAT(args[0]);
    return xr_float(log(v));
}

static XrValue math_log10(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = XR_TO_FLOAT(args[0]);
    return xr_float(log10(v));
}

static XrValue math_log2(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = XR_TO_FLOAT(args[0]);
    return xr_float(log2(v));
}

static XrValue math_exp(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = XR_TO_FLOAT(args[0]);
    return xr_float(exp(v));
}

/* ========== Hyperbolic ========== */

static XrValue math_sinh(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(sinh(XR_TO_FLOAT(args[0])));
}

static XrValue math_cosh(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(1.0);
    return xr_float(cosh(XR_TO_FLOAT(args[0])));
}

static XrValue math_tanh(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(tanh(XR_TO_FLOAT(args[0])));
}

/* ========== Additional Math ========== */

static XrValue math_hypot(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_float(0.0);
    return xr_float(hypot(XR_TO_FLOAT(args[0]), XR_TO_FLOAT(args[1])));
}

static XrValue math_cbrt(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(cbrt(XR_TO_FLOAT(args[0])));
}

static XrValue math_trunc(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(0);
    if (XR_IS_INT(args[0]))
        return args[0];
    double result = trunc(XR_TO_FLOAT(args[0]));
    if (DOUBLE_FITS_INT64(result))
        return xr_int((int64_t) result);
    return xr_float(result);
}

static XrValue math_fmod(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_float(0.0);
    return xr_float(fmod(XR_TO_FLOAT(args[0]), XR_TO_FLOAT(args[1])));
}

static XrValue math_log1p(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(log1p(XR_TO_FLOAT(args[0])));
}

static XrValue math_expm1(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(expm1(XR_TO_FLOAT(args[0])));
}

/* ========== Comparison ========== */

/* ========== Random ========== */

static XrValue math_random(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;
    return xr_float(xr_math_core_random_f64(xr_random_bytes_callback, NULL));
}

static XrValue math_randomInt(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_int(0);

    int64_t min_val =
        xr_math_core_int_arg_or(XR_IS_INT(args[0]), XR_IS_INT(args[0]) ? XR_TO_INT(args[0]) : 0, 0);
    int64_t max_val =
        xr_math_core_int_arg_or(XR_IS_INT(args[1]), XR_IS_INT(args[1]) ? XR_TO_INT(args[1]) : 0, 0);

    return xr_int(xr_math_core_random_i64(xr_random_bytes_callback, NULL, min_val, max_val));
}

/* ========== Utilities ========== */

#define XR_STDLIB_VM_BIND_MODULE_MATH 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_MATH
