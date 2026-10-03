/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_fault.h - Allocation-free runtime fault detail
 *
 * KEY CONCEPT:
 *   A fault retains observed operands independently of every runtime owner.
 *   Every panic-channel fault names its runtime error code; other faults
 *   (resource limits, cancellation, invalid state) carry an all-zero detail.
 */
#ifndef XXIR_FAULT_H
#define XXIR_FAULT_H
#include <stdbool.h>
#include <stdint.h>

typedef struct XrXirFaultDetail {
    uint32_t code, reserved;
    int64_t index, length;
} XrXirFaultDetail;

#define XR_XIR_PANIC_DIVIDE 420u
#define XR_XIR_PANIC_REMAINDER 421u
#define XR_XIR_PANIC_RANGE 422u
#define XR_XIR_PANIC_NULL_UNWRAP 413u
#define XR_XIR_PANIC_BOUNDS 430u
#define XR_XIR_PANIC_MATCH 442u
#define XR_XIR_PANIC_DEFER_ASYNC 444u
#define XR_XIR_PANIC_ASSERTION 445u

static inline bool xr_xir_fault_empty(XrXirFaultDetail fault) {
    return !fault.code && !fault.reserved && !fault.index && !fault.length;
}
static inline bool xr_xir_fault_bounds_valid(XrXirFaultDetail fault) {
    return fault.code == XR_XIR_PANIC_BOUNDS && !fault.reserved && fault.length >= 0 &&
        (fault.index < 0 || fault.index >= fault.length);
}
static inline bool xr_xir_fault_match_valid(XrXirFaultDetail fault) {
    return fault.code == XR_XIR_PANIC_MATCH && !fault.reserved && !fault.index && !fault.length;
}
/* Integer division and remainder by zero keep their distinct codes. */
static inline bool xr_xir_fault_divide_valid(XrXirFaultDetail fault) {
    return (fault.code == XR_XIR_PANIC_DIVIDE || fault.code == XR_XIR_PANIC_REMAINDER) &&
        !fault.reserved && !fault.index && !fault.length;
}
static inline bool xr_xir_fault_range_valid(XrXirFaultDetail fault) {
    return fault.code == XR_XIR_PANIC_RANGE && !fault.reserved && !fault.index && !fault.length;
}
static inline bool xr_xir_fault_defer_async_valid(XrXirFaultDetail fault) {
    return fault.code == XR_XIR_PANIC_DEFER_ASYNC && !fault.reserved && !fault.index && !fault.length;
}
/* Runtime panics identified by their code alone share one call status. */
static inline bool xr_xir_fault_runtime_valid(XrXirFaultDetail fault) {
    return fault.code == XR_XIR_PANIC_NULL_UNWRAP && !fault.reserved && !fault.index && !fault.length;
}
static inline bool xr_xir_fault_panic_valid(XrXirFaultDetail fault) {
    return xr_xir_fault_runtime_valid(fault) || xr_xir_fault_divide_valid(fault) || xr_xir_fault_range_valid(fault) ||
        xr_xir_fault_bounds_valid(fault) || xr_xir_fault_match_valid(fault) || xr_xir_fault_defer_async_valid(fault) ||
        (fault.code == XR_XIR_PANIC_ASSERTION && !fault.reserved && !fault.index && !fault.length);
}
#endif // XXIR_FAULT_H
