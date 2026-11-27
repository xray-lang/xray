/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_float.c - Hardware binary64 operations with checked integer conversion
 *
 * KEY CONCEPT:
 *   Callers provide nearest-even rounding with gradual underflow. Arithmetic
 *   leaves control modes untouched; hardware exception flags and NaN payloads
 *   are not preserved. Integer range failures keep their execution location.
 */
#include "xray_native_runtime.h"
#include "base/xchecks.h"
#include "os/os_random.h"
#include "shared/xr_math_core.h"
#include "shared/xr_math_host_ops.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#if defined(_M_FP_FAST) || defined(__FAST_MATH__) || \
    (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error HARDWARE_F64_REQUIRES_NONFAST_FLOATING_POINT_MODE
#endif
XR_STATIC_ASSERT(CHAR_BIT == 8 && sizeof(double) == 8 && sizeof(uint64_t) == 8 && FLT_RADIX == 2 &&
    DBL_MANT_DIG == 53 && DBL_MIN_EXP == -1021 && DBL_MAX_EXP == 1024,
    "The native runtime requires binary64 double");
static double native_float_value(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static uint64_t native_float_bits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}
XRAY_API uint64_t xrt_f64_add(uint64_t left, uint64_t right) {
    return native_float_bits(native_float_value(left) + native_float_value(right));
}
XRAY_API uint64_t xrt_f64_subtract(uint64_t left, uint64_t right) {
    return native_float_bits(native_float_value(left) - native_float_value(right));
}
XRAY_API uint64_t xrt_f64_multiply(uint64_t left, uint64_t right) {
    return native_float_bits(native_float_value(left) * native_float_value(right));
}
XRAY_API uint64_t xrt_f64_divide(uint64_t left, uint64_t right) {
    return native_float_bits(native_float_value(left) / native_float_value(right));
}
XRAY_API int32_t xrt_f64_compare(uint64_t left, uint64_t right) {
    double a = native_float_value(left), b = native_float_value(right);
    return a < b ? -1 : a > b ? 1 : a == b ? 0 : 2;
}
XRAY_API uint64_t xrt_i64_to_f64(int64_t value) {
    return native_float_bits((double)value);
}
XRAY_API int64_t xrt_f64_to_i64(uint64_t bits, uint32_t source_id) {
    double value = native_float_value(bits);
    /* Both endpoints are exactly representable. The conjunction also rejects
     * every NaN before an out-of-range C conversion can become undefined. */
    if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0))
        xrt_arithmetic_panic(422, source_id);
    return (int64_t)value;
}

#define XR_MATH_HOST_BODY_1(name) \
    XRAY_API double xrt_f64_##name(double value, uint32_t source_id) { \
        (void)source_id; \
        return name(value); \
    }
#define XR_MATH_HOST_BODY_2(name) \
    XRAY_API double xrt_f64_##name(double left, double right, uint32_t source_id) { \
        (void)source_id; \
        return name(left, right); \
    }
#define XR_MATH_HOST_BODY(tag, name, arity) XR_MATH_HOST_BODY_##arity(name)
XR_MATH_HOST_OPERATIONS(XR_MATH_HOST_BODY)
#undef XR_MATH_HOST_BODY
#undef XR_MATH_HOST_BODY_2
#undef XR_MATH_HOST_BODY_1

XRAY_API double xrt_math_random(uint32_t source_id) {
    (void)source_id;
    return xr_math_core_random_f64(xr_random_bytes_callback, NULL);
}

XRAY_API int64_t xrt_math_random_int(int64_t minimum, int64_t maximum, uint32_t source_id) {
    (void)source_id;
    return xr_math_core_random_i64(xr_random_bytes_callback, NULL, minimum, maximum);
}
