/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_time.h - Freestanding AOT helpers for pure time queries.
 */

#ifndef XRT_TIME_H
#define XRT_TIME_H

#include "xrt_value.h"
#include "../os/os_time.h"

static inline XrValue xrt_time_now(void) {
    return XR_FROM_INT((int64_t) (xr_time_realtime_ns() / 1000000ULL));
}

static inline XrValue xrt_time_monotonic(void) {
    return XR_FROM_INT((int64_t) (xr_time_monotonic_ns() / 1000000ULL));
}

static inline XrValue xrt_time_nanos(void) {
    return XR_FROM_INT((int64_t) xr_time_monotonic_ns());
}

static inline XrValue xrt_time_micros(void) {
    return XR_FROM_INT((int64_t) (xr_time_monotonic_ns() / 1000ULL));
}

static inline XrValue xrt_time_clock(void) {
    return XR_FROM_INT((int64_t) (xr_time_process_cpu_ns() / 1000000ULL));
}

#endif  // XRT_TIME_H
