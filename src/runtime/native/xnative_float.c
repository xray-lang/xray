/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_float.c - Canonical binary64 operations for native bit consumers
 *
 * KEY CONCEPT:
 *   Bit operations share the integer-only kernel without depending on the
 *   caller's floating environment. Failed integer conversions use the active
 *   execution's arithmetic panic owner and source location.
 */
#include "xray_native_runtime.h"
#include "base/xchecks.h"
#include "xir/xxir_float.h"

static uint64_t native_float_binary(XrXirFloatOperation operation, uint64_t left, uint64_t right) {
    uint64_t result = 0;
    XrXirNumericStatus status = xr_xir_float_arithmetic(64, operation, left, right, &result);
    XR_CHECK(status == XR_XIR_NUMERIC_OK, "closed binary64 operation accepts every operand bit pattern");
    return result;
}
XRAY_API uint64_t xrt_f64_add(uint64_t left, uint64_t right) {
    return native_float_binary(XR_XIR_FLOAT_ADD, left, right);
}
XRAY_API uint64_t xrt_f64_subtract(uint64_t left, uint64_t right) {
    return native_float_binary(XR_XIR_FLOAT_SUBTRACT, left, right);
}
XRAY_API uint64_t xrt_f64_multiply(uint64_t left, uint64_t right) {
    return native_float_binary(XR_XIR_FLOAT_MULTIPLY, left, right);
}
XRAY_API uint64_t xrt_f64_divide(uint64_t left, uint64_t right) {
    return native_float_binary(XR_XIR_FLOAT_DIVIDE, left, right);
}
XRAY_API int32_t xrt_f64_compare(uint64_t left, uint64_t right) {
    XR_STATIC_ASSERT(XR_XIR_FLOAT_LESS == -1 && XR_XIR_FLOAT_EQUAL == 0 &&
        XR_XIR_FLOAT_GREATER == 1 && XR_XIR_FLOAT_UNORDERED == 2, "public binary64 ordering matches the canonical kernel");
    XrXirFloatOrder order;
    XrXirNumericStatus status = xr_xir_float_compare(64, left, right, &order);
    XR_CHECK(status == XR_XIR_NUMERIC_OK, "binary64 comparison accepts every operand bit pattern");
    return (int32_t) order;
}
XRAY_API uint64_t xrt_i64_to_f64(int64_t value) {
    uint64_t result = 0;
    XrXirNumericStatus status = xr_xir_integer_to_float(
        (XrXirIntegerFormat){64, true}, 64, value, &result);
    XR_CHECK(status == XR_XIR_NUMERIC_OK, "every signed64 value has a binary64 conversion");
    return result;
}
XRAY_API int64_t xrt_f64_to_i64(uint64_t bits, uint32_t source_id) {
    int64_t result = 0;
    XrXirNumericStatus status = xr_xir_float_to_integer(
        64, (XrXirIntegerFormat){64, true}, bits, &result);
    if (status == XR_XIR_NUMERIC_RANGE) xrt_arithmetic_panic(422, source_id);
    XR_CHECK(status == XR_XIR_NUMERIC_OK, "binary64 conversion uses a valid signed64 format");
    return result;
}
