/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * math.c - Math standard library implementation
 *
 * KEY CONCEPT:
 *   Thin wrappers over C math.h functions, exposed to xray scripts.
 *   All numeric functions accept both int and float arguments.
 */

#include "math.h"
#include "../common.h"
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <float.h>
#include "../../include/xray_platform.h"
#include "../../src/base/xchecks.h"

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

// Return a NaN for non-numeric inputs instead of silently clamping to 0.
// `math.sqrt("foo")` previously produced `sqrt(0) == 0`, which hid bugs at
// the call site. NaN propagation makes the failure observable and matches
// the standard IEEE-754 contract.
static double get_number(XrValue v) {
    if (XR_IS_INT(v))
        return (double) XR_TO_INT(v);
    if (XR_IS_FLOAT(v))
        return XR_TO_FLOAT(v);
    return NAN;
}

/* ========== Basic Math ========== */

static XrValue math_abs(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(0);
    if (XR_IS_INT(args[0])) {
        int64_t v = XR_TO_INT(args[0]);
        // INT64_MIN overflow: |INT64_MIN| = 2^63, exceeds INT64_MAX
        if (v == INT64_MIN)
            return xr_float((double) INT64_MAX + 1.0);
        return xr_int(v < 0 ? -v : v);
    }
    return xr_float(fabs(get_number(args[0])));
}

static XrValue math_floor(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(0);
    if (XR_IS_INT(args[0]))
        return args[0];
    double result = floor(get_number(args[0]));
    if (DOUBLE_FITS_INT64(result))
        return xr_int((int64_t) result);
    return xr_float(result);
}

static XrValue math_ceil(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(0);
    if (XR_IS_INT(args[0]))
        return args[0];
    double result = ceil(get_number(args[0]));
    if (DOUBLE_FITS_INT64(result))
        return xr_int((int64_t) result);
    return xr_float(result);
}

static XrValue math_round(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(0);
    if (XR_IS_INT(args[0]))
        return args[0];
    double result = round(get_number(args[0]));
    if (DOUBLE_FITS_INT64(result))
        return xr_int((int64_t) result);
    return xr_float(result);
}

static XrValue math_sqrt(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(sqrt(v));
}

static XrValue math_pow(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_float(0.0);
    double x = get_number(args[0]);
    double y = get_number(args[1]);
    return xr_float(pow(x, y));
}

/* ========== Trigonometric ========== */

static XrValue math_sin(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(sin(v));
}

static XrValue math_cos(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(cos(v));
}

static XrValue math_tan(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(tan(v));
}

static XrValue math_asin(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(asin(v));
}

static XrValue math_acos(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(acos(v));
}

static XrValue math_atan(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(atan(v));
}

static XrValue math_atan2(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_float(0.0);
    double y = get_number(args[0]);
    double x = get_number(args[1]);
    return xr_float(atan2(y, x));
}

/* ========== Logarithmic & Exponential ========== */

static XrValue math_log(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(log(v));
}

static XrValue math_log10(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(log10(v));
}

static XrValue math_log2(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(log2(v));
}

static XrValue math_exp(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(exp(v));
}

/* ========== Hyperbolic ========== */

static XrValue math_sinh(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(sinh(get_number(args[0])));
}

static XrValue math_cosh(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(1.0);
    return xr_float(cosh(get_number(args[0])));
}

static XrValue math_tanh(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(tanh(get_number(args[0])));
}

/* ========== Additional Math ========== */

static XrValue math_hypot(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_float(0.0);
    return xr_float(hypot(get_number(args[0]), get_number(args[1])));
}

static XrValue math_cbrt(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(cbrt(get_number(args[0])));
}

static XrValue math_trunc(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(0);
    if (XR_IS_INT(args[0]))
        return args[0];
    double result = trunc(get_number(args[0]));
    if (DOUBLE_FITS_INT64(result))
        return xr_int((int64_t) result);
    return xr_float(result);
}

static XrValue math_fmod(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_float(0.0);
    return xr_float(fmod(get_number(args[0]), get_number(args[1])));
}

static XrValue math_log1p(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(log1p(get_number(args[0])));
}

static XrValue math_expm1(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(expm1(get_number(args[0])));
}

static XrValue math_lerp(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 3)
        return xr_float(0.0);
    double a = get_number(args[0]);
    double b = get_number(args[1]);
    double t = get_number(args[2]);
    return xr_float(a + (b - a) * t);
}

static XrValue math_degToRad(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(get_number(args[0]) * (M_PI / 180.0));
}

static XrValue math_radToDeg(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(get_number(args[0]) * (180.0 / M_PI));
}

/* ========== Comparison ========== */

static XrValue math_min(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return argc == 1 ? args[0] : xr_null();
    bool all_int = true;
    for (int i = 0; i < argc; i++) {
        if (!XR_IS_INT(args[i])) {
            all_int = false;
            break;
        }
    }
    if (all_int) {
        int64_t result = XR_TO_INT(args[0]);
        for (int i = 1; i < argc; i++) {
            int64_t v = XR_TO_INT(args[i]);
            if (v < result)
                result = v;
        }
        return xr_int(result);
    }
    double result = get_number(args[0]);
    for (int i = 1; i < argc; i++) {
        double v = get_number(args[i]);
        if (v < result)
            result = v;
    }
    return xr_float(result);
}

static XrValue math_max(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return argc == 1 ? args[0] : xr_null();
    bool all_int = true;
    for (int i = 0; i < argc; i++) {
        if (!XR_IS_INT(args[i])) {
            all_int = false;
            break;
        }
    }
    if (all_int) {
        int64_t result = XR_TO_INT(args[0]);
        for (int i = 1; i < argc; i++) {
            int64_t v = XR_TO_INT(args[i]);
            if (v > result)
                result = v;
        }
        return xr_int(result);
    }
    double result = get_number(args[0]);
    for (int i = 1; i < argc; i++) {
        double v = get_number(args[i]);
        if (v > result)
            result = v;
    }
    return xr_float(result);
}

static XrValue math_clamp(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 3)
        return xr_null();
    if (XR_IS_INT(args[0]) && XR_IS_INT(args[1]) && XR_IS_INT(args[2])) {
        int64_t x = XR_TO_INT(args[0]);
        int64_t lo = XR_TO_INT(args[1]);
        int64_t hi = XR_TO_INT(args[2]);
        if (x < lo)
            return xr_int(lo);
        if (x > hi)
            return xr_int(hi);
        return xr_int(x);
    }
    double x = get_number(args[0]);
    double lo = get_number(args[1]);
    double hi = get_number(args[2]);
    if (x < lo)
        return xr_float(lo);
    if (x > hi)
        return xr_float(hi);
    return xr_float(x);
}

/* ========== Random ========== */

static XrValue math_random(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;
    uint64_t r;
    xr_random_bytes((unsigned char *) &r, sizeof(r));
    // Top 53 bits → full double mantissa precision, result in [0, 1)
    return xr_float((r >> 11) * (1.0 / ((uint64_t) 1 << 53)));
}

static XrValue math_randomInt(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_int(0);

    int64_t min_val = XR_IS_INT(args[0]) ? XR_TO_INT(args[0]) : (int64_t) get_number(args[0]);
    int64_t max_val = XR_IS_INT(args[1]) ? XR_TO_INT(args[1]) : (int64_t) get_number(args[1]);

    if (min_val > max_val) {
        int64_t tmp = min_val;
        min_val = max_val;
        max_val = tmp;
    }
    if (min_val == max_val)
        return xr_int(min_val);

    // Use unsigned arithmetic to avoid overflow when range spans full int64
    uint64_t range = (uint64_t) (max_val - min_val) + 1;
    uint64_t r;

    if (range == 0) {
        // Full 2^64 range (min=INT64_MIN, max=INT64_MAX)
        xr_random_bytes((unsigned char *) &r, sizeof(r));
    } else {
        // Rejection sampling to eliminate modulo bias
        uint64_t threshold = (-range) % range;
        do {
            xr_random_bytes((unsigned char *) &r, sizeof(r));
        } while (r < threshold);
        r = r % range;
    }
    return xr_int(min_val + (int64_t) r);
}

/* ========== Utilities ========== */

static XrValue math_sign(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(0);
    double v = get_number(args[0]);
    if (v > 0)
        return xr_int(1);
    if (v < 0)
        return xr_int(-1);
    return xr_int(0);
}

static XrValue math_isNaN(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    if (!XR_IS_FLOAT(args[0]))
        return xr_bool(false);
    double v = XR_TO_FLOAT(args[0]);
    return xr_bool(isnan(v));
}

static XrValue math_isFinite(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    double v = get_number(args[0]);
    return xr_bool(isfinite(v));
}

/* ========== Type Declarations (parsed by gen_stdlib_types.py) ========== */

#include "../../src/module/xbuiltin_decl.h"

// @module math

XR_DEFINE_BUILTIN(math_abs, "abs", "(x: float): float", "Absolute value (preserves int)")
XR_DEFINE_BUILTIN(math_floor, "floor", "(x: float): int", "Floor to integer")
XR_DEFINE_BUILTIN(math_ceil, "ceil", "(x: float): int", "Ceiling to integer")
XR_DEFINE_BUILTIN(math_round, "round", "(x: float): int", "Round to nearest integer")
XR_DEFINE_BUILTIN(math_sqrt, "sqrt", "(x: float): float", "Square root")
XR_DEFINE_BUILTIN(math_pow, "pow", "(base: float, exp: float): float", "Power")
XR_DEFINE_BUILTIN(math_sin, "sin", "(x: float): float", "Sine")
XR_DEFINE_BUILTIN(math_cos, "cos", "(x: float): float", "Cosine")
XR_DEFINE_BUILTIN(math_tan, "tan", "(x: float): float", "Tangent")
XR_DEFINE_BUILTIN(math_asin, "asin", "(x: float): float", "Arc sine")
XR_DEFINE_BUILTIN(math_acos, "acos", "(x: float): float", "Arc cosine")
XR_DEFINE_BUILTIN(math_atan, "atan", "(x: float): float", "Arc tangent")
XR_DEFINE_BUILTIN(math_atan2, "atan2", "(y: float, x: float): float", "Arc tangent of y/x")
XR_DEFINE_BUILTIN(math_log, "log", "(x: float): float", "Natural logarithm")
XR_DEFINE_BUILTIN(math_log10, "log10", "(x: float): float", "Base-10 logarithm")
XR_DEFINE_BUILTIN(math_log2, "log2", "(x: float): float", "Base-2 logarithm")
XR_DEFINE_BUILTIN(math_exp, "exp", "(x: float): float", "Exponential e^x")
XR_DEFINE_BUILTIN(math_min, "min", "(...args: float): float", "Minimum (preserves int)")
XR_DEFINE_BUILTIN(math_max, "max", "(...args: float): float", "Maximum (preserves int)")
XR_DEFINE_BUILTIN(math_clamp, "clamp", "(x: float, min: float, max: float): float",
                  "Clamp (preserves int)")
XR_DEFINE_BUILTIN(math_random, "random", "(): float", "Random float in [0, 1)")
XR_DEFINE_BUILTIN(math_randomInt, "randomInt", "(min: int, max: int): int",
                  "Random integer in [min, max]")
XR_DEFINE_BUILTIN(math_sign, "sign", "(x: float): int", "Sign of value (-1, 0, 1)")
XR_DEFINE_BUILTIN(math_sinh, "sinh", "(x: float): float", "Hyperbolic sine")
XR_DEFINE_BUILTIN(math_cosh, "cosh", "(x: float): float", "Hyperbolic cosine")
XR_DEFINE_BUILTIN(math_tanh, "tanh", "(x: float): float", "Hyperbolic tangent")
XR_DEFINE_BUILTIN(math_hypot, "hypot", "(x: float, y: float): float", "Hypotenuse sqrt(x*x+y*y)")
XR_DEFINE_BUILTIN(math_cbrt, "cbrt", "(x: float): float", "Cube root")
XR_DEFINE_BUILTIN(math_trunc, "trunc", "(x: float): int", "Truncate toward zero")
XR_DEFINE_BUILTIN(math_fmod, "fmod", "(x: float, y: float): float", "Floating-point remainder")
XR_DEFINE_BUILTIN(math_log1p, "log1p", "(x: float): float", "log(1+x) accurate for small x")
XR_DEFINE_BUILTIN(math_expm1, "expm1", "(x: float): float", "exp(x)-1 accurate for small x")
XR_DEFINE_BUILTIN(math_lerp, "lerp", "(a: float, b: float, t: float): float",
                  "Linear interpolation")
XR_DEFINE_BUILTIN(math_degToRad, "degToRad", "(deg: float): float", "Degrees to radians")
XR_DEFINE_BUILTIN(math_radToDeg, "radToDeg", "(rad: float): float", "Radians to degrees")
XR_DEFINE_BUILTIN(math_isNaN, "isNaN", "(x: float): bool", "Check if NaN")
XR_DEFINE_BUILTIN(math_isFinite, "isFinite", "(x: float): bool", "Check if finite")

/* ========== Module Loading ========== */

XrModule *xr_load_module_math(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_math: NULL isolate");

    XrModule *mod = xr_module_create_native(isolate, "math");
    if (!mod)
        return NULL;

    XRS_EXPORT(mod, isolate, "abs", math_abs);
    XRS_EXPORT(mod, isolate, "floor", math_floor);
    XRS_EXPORT(mod, isolate, "ceil", math_ceil);
    XRS_EXPORT(mod, isolate, "round", math_round);
    XRS_EXPORT(mod, isolate, "sqrt", math_sqrt);
    XRS_EXPORT(mod, isolate, "pow", math_pow);
    XRS_EXPORT(mod, isolate, "sin", math_sin);
    XRS_EXPORT(mod, isolate, "cos", math_cos);
    XRS_EXPORT(mod, isolate, "tan", math_tan);
    XRS_EXPORT(mod, isolate, "asin", math_asin);
    XRS_EXPORT(mod, isolate, "acos", math_acos);
    XRS_EXPORT(mod, isolate, "atan", math_atan);
    XRS_EXPORT(mod, isolate, "atan2", math_atan2);
    XRS_EXPORT(mod, isolate, "log", math_log);
    XRS_EXPORT(mod, isolate, "log10", math_log10);
    XRS_EXPORT(mod, isolate, "log2", math_log2);
    XRS_EXPORT(mod, isolate, "exp", math_exp);
    XRS_EXPORT(mod, isolate, "min", math_min);
    XRS_EXPORT(mod, isolate, "max", math_max);
    XRS_EXPORT(mod, isolate, "clamp", math_clamp);
    XRS_EXPORT(mod, isolate, "random", math_random);
    XRS_EXPORT(mod, isolate, "randomInt", math_randomInt);
    XRS_EXPORT(mod, isolate, "sign", math_sign);
    XRS_EXPORT(mod, isolate, "sinh", math_sinh);
    XRS_EXPORT(mod, isolate, "cosh", math_cosh);
    XRS_EXPORT(mod, isolate, "tanh", math_tanh);
    XRS_EXPORT(mod, isolate, "hypot", math_hypot);
    XRS_EXPORT(mod, isolate, "cbrt", math_cbrt);
    XRS_EXPORT(mod, isolate, "trunc", math_trunc);
    XRS_EXPORT(mod, isolate, "fmod", math_fmod);
    XRS_EXPORT(mod, isolate, "log1p", math_log1p);
    XRS_EXPORT(mod, isolate, "expm1", math_expm1);
    XRS_EXPORT(mod, isolate, "lerp", math_lerp);
    XRS_EXPORT(mod, isolate, "degToRad", math_degToRad);
    XRS_EXPORT(mod, isolate, "radToDeg", math_radToDeg);
    XRS_EXPORT(mod, isolate, "isNaN", math_isNaN);
    XRS_EXPORT(mod, isolate, "isFinite", math_isFinite);

    // Constants
    xr_module_add_export(isolate, mod, "PI", xr_float(M_PI));
    xr_module_add_export(isolate, mod, "E", xr_float(M_E));
    xr_module_add_export(isolate, mod, "TAU", xr_float(2.0 * M_PI));
    xr_module_add_export(isolate, mod, "SQRT2", xr_float(M_SQRT2));
    xr_module_add_export(isolate, mod, "LN2", xr_float(M_LN2));
    xr_module_add_export(isolate, mod, "LN10", xr_float(M_LN10));
    xr_module_add_export(isolate, mod, "LOG2E", xr_float(M_LOG2E));
    xr_module_add_export(isolate, mod, "LOG10E", xr_float(M_LOG10E));
    xr_module_add_export(isolate, mod, "EPSILON", xr_float(DBL_EPSILON));
    xr_module_add_export(isolate, mod, "MAX_INT", xr_int(INT64_MAX));
    xr_module_add_export(isolate, mod, "MIN_INT", xr_int(INT64_MIN));
    xr_module_add_export(isolate, mod, "MAX_FLOAT", xr_float(DBL_MAX));
    xr_module_add_export(isolate, mod, "INF", xr_float(INFINITY));
    xr_module_add_export(isolate, mod, "NAN", xr_float(NAN));

    mod->loaded = true;
    return mod;
}
