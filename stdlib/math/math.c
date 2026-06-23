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
#include "../../src/os/os_random.h"
#include "../../src/shared/xr_math_core.h"
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

static XrValue math_abs(XrVMRuntime *X, XrValue *args, int argc) {
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

static XrValue math_floor(XrVMRuntime *X, XrValue *args, int argc) {
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

static XrValue math_ceil(XrVMRuntime *X, XrValue *args, int argc) {
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

static XrValue math_round(XrVMRuntime *X, XrValue *args, int argc) {
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

static XrValue math_sqrt(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(sqrt(v));
}

static XrValue math_pow(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_float(0.0);
    double x = get_number(args[0]);
    double y = get_number(args[1]);
    return xr_float(pow(x, y));
}

/* ========== Trigonometric ========== */

static XrValue math_sin(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(sin(v));
}

static XrValue math_cos(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(cos(v));
}

static XrValue math_tan(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(tan(v));
}

static XrValue math_asin(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(asin(v));
}

static XrValue math_acos(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(acos(v));
}

static XrValue math_atan(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(atan(v));
}

static XrValue math_atan2(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_float(0.0);
    double y = get_number(args[0]);
    double x = get_number(args[1]);
    return xr_float(atan2(y, x));
}

/* ========== Logarithmic & Exponential ========== */

static XrValue math_log(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(log(v));
}

static XrValue math_log10(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(log10(v));
}

static XrValue math_log2(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(log2(v));
}

static XrValue math_exp(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    double v = get_number(args[0]);
    return xr_float(exp(v));
}

/* ========== Hyperbolic ========== */

static XrValue math_sinh(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(sinh(get_number(args[0])));
}

static XrValue math_cosh(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(1.0);
    return xr_float(cosh(get_number(args[0])));
}

static XrValue math_tanh(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(tanh(get_number(args[0])));
}

/* ========== Additional Math ========== */

static XrValue math_hypot(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_float(0.0);
    return xr_float(hypot(get_number(args[0]), get_number(args[1])));
}

static XrValue math_cbrt(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(cbrt(get_number(args[0])));
}

static XrValue math_trunc(XrVMRuntime *X, XrValue *args, int argc) {
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

static XrValue math_fmod(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_float(0.0);
    return xr_float(fmod(get_number(args[0]), get_number(args[1])));
}

static XrValue math_log1p(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(log1p(get_number(args[0])));
}

static XrValue math_expm1(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(expm1(get_number(args[0])));
}

static XrValue math_lerp(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 3)
        return xr_float(0.0);
    double a = get_number(args[0]);
    double b = get_number(args[1]);
    double t = get_number(args[2]);
    return xr_float(a + (b - a) * t);
}

static XrValue math_degToRad(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(get_number(args[0]) * (M_PI / 180.0));
}

static XrValue math_radToDeg(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_float(0.0);
    return xr_float(get_number(args[0]) * (180.0 / M_PI));
}

/* ========== Comparison ========== */

static XrValue math_min(XrVMRuntime *X, XrValue *args, int argc) {
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
    /* IEEE-754 NaN propagation: any NaN argument produces NaN. */
    double result = get_number(args[0]);
    if (isnan(result))
        return xr_float(NAN);
    for (int i = 1; i < argc; i++) {
        double v = get_number(args[i]);
        if (isnan(v))
            return xr_float(NAN);
        if (v < result)
            result = v;
    }
    return xr_float(result);
}

static XrValue math_max(XrVMRuntime *X, XrValue *args, int argc) {
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
    /* IEEE-754 NaN propagation: any NaN argument produces NaN. */
    double result = get_number(args[0]);
    if (isnan(result))
        return xr_float(NAN);
    for (int i = 1; i < argc; i++) {
        double v = get_number(args[i]);
        if (isnan(v))
            return xr_float(NAN);
        if (v > result)
            result = v;
    }
    return xr_float(result);
}

static XrValue math_clamp(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 3)
        return xr_null();
    if (XR_IS_INT(args[0]) && XR_IS_INT(args[1]) && XR_IS_INT(args[2])) {
        int64_t x = XR_TO_INT(args[0]);
        int64_t lo = XR_TO_INT(args[1]);
        int64_t hi = XR_TO_INT(args[2]);
        if (lo > hi) {
            int64_t tmp = lo;
            lo = hi;
            hi = tmp;
        }
        if (x < lo)
            return xr_int(lo);
        if (x > hi)
            return xr_int(hi);
        return xr_int(x);
    }
    double x = get_number(args[0]);
    double lo = get_number(args[1]);
    double hi = get_number(args[2]);
    if (isnan(x) || isnan(lo) || isnan(hi))
        return xr_float(NAN);
    if (lo > hi) {
        double tmp = lo;
        lo = hi;
        hi = tmp;
    }
    if (x < lo)
        return xr_float(lo);
    if (x > hi)
        return xr_float(hi);
    return xr_float(x);
}

/* ========== Random ========== */

static void math_random_bytes(void *ctx, unsigned char *buf, size_t len) {
    (void) ctx;
    xr_random_bytes(buf, len);
}

static XrValue math_random(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;
    return xr_float(xr_math_core_random_f64(math_random_bytes, NULL));
}

static XrValue math_randomInt(XrVMRuntime *X, XrValue *args, int argc) {
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

    return xr_int(xr_math_core_random_i64(math_random_bytes, NULL, min_val, max_val));
}

/* ========== Utilities ========== */

static XrValue math_sign(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(0);
    double v = get_number(args[0]);
    if (isnan(v))
        return xr_float(NAN);
    if (v > 0)
        return xr_int(1);
    if (v < 0)
        return xr_int(-1);
    return xr_int(0);
}

static XrValue math_isNaN(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    if (!XR_IS_FLOAT(args[0]))
        return xr_bool(false);
    double v = XR_TO_FLOAT(args[0]);
    return xr_bool(isnan(v));
}

static XrValue math_isFinite(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    if (XR_IS_INT(args[0]))
        return xr_bool(true); /* integers are always finite */
    double v = get_number(args[0]);
    return xr_bool(isfinite(v));
}

#define XR_STDLIB_VM_BIND_MODULE_MATH 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_MATH

/* ========== Module Loading ========== */

XR_FUNC XrModule *xr_load_module_math(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_math: NULL isolate");

    XrModule *mod = xr_module_create_native(isolate, "math");
    if (!mod)
        return NULL;

    xr_stdlib_vm_bind_math_generated(isolate, mod);

    mod->loaded = true;
    return mod;
}
