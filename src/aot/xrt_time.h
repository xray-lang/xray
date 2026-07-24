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
#include "../shared/xr_time_offset.h"
#include <stdint.h>
#include <time.h>

#if !defined(XRAY_PROFILE_FREESTANDING)

/* Cross-target AOT time queries are header-only so the target binary never
 * consumes the compiler host's xray_aot_core archive. Native builds retain the
 * shared OS clock implementation used by the runtime and scheduler. */
#if !defined(XR_AOT_CROSS_TARGET)

#include "../os/os_time.h"

static inline uint64_t xrt_platform_monotonic_ns(void) {
    return xr_time_monotonic_ns();
}

static inline uint64_t xrt_platform_realtime_ns(void) {
    return xr_time_realtime_ns();
}

static inline uint64_t xrt_platform_process_cpu_ns(void) {
    return xr_time_process_cpu_ns();
}

#else

#if defined(XR_OS_WINDOWS)

static inline uint64_t xrt_platform_monotonic_ns(void) {
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    uint64_t freq = (uint64_t) frequency.QuadPart;
    uint64_t whole_seconds = (uint64_t) counter.QuadPart / freq;
    uint64_t remainder = (uint64_t) counter.QuadPart % freq;
    return whole_seconds * 1000000000ULL + (remainder * 1000000000ULL) / freq;
}

static inline uint64_t xrt_platform_realtime_ns(void) {
    FILETIME file_time;
    ULARGE_INTEGER ticks;
    GetSystemTimePreciseAsFileTime(&file_time);
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    return (ticks.QuadPart - 116444736000000000ULL) * 100ULL;
}

static inline uint64_t xrt_platform_process_cpu_ns(void) {
    FILETIME creation;
    FILETIME exit;
    FILETIME kernel;
    FILETIME user;
    ULARGE_INTEGER kernel_ticks;
    ULARGE_INTEGER user_ticks;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return 0;
    kernel_ticks.LowPart = kernel.dwLowDateTime;
    kernel_ticks.HighPart = kernel.dwHighDateTime;
    user_ticks.LowPart = user.dwLowDateTime;
    user_ticks.HighPart = user.dwHighDateTime;
    return (kernel_ticks.QuadPart + user_ticks.QuadPart) * 100ULL;
}

#else

static inline uint64_t xrt_platform_timespec_ns(clockid_t clock_id) {
    struct timespec value;
    if (clock_gettime(clock_id, &value) != 0)
        return 0;
    return (uint64_t) value.tv_sec * 1000000000ULL + (uint64_t) value.tv_nsec;
}

static inline uint64_t xrt_platform_monotonic_ns(void) {
    return xrt_platform_timespec_ns(CLOCK_MONOTONIC);
}

static inline uint64_t xrt_platform_realtime_ns(void) {
    return xrt_platform_timespec_ns(CLOCK_REALTIME);
}

static inline uint64_t xrt_platform_process_cpu_ns(void) {
#if defined(CLOCK_PROCESS_CPUTIME_ID)
    return xrt_platform_timespec_ns(CLOCK_PROCESS_CPUTIME_ID);
#else
    clock_t value = clock();
    return (uint64_t) value * (1000000000ULL / CLOCKS_PER_SEC);
#endif
}

#endif

#endif

static inline XrValue xrt_time_now(void) {
    return XR_FROM_INT((int64_t) (xrt_platform_realtime_ns() / 1000000ULL));
}

static inline XrValue xrt_time_monotonic(void) {
    return XR_FROM_INT((int64_t) (xrt_platform_monotonic_ns() / 1000000ULL));
}

static inline XrValue xrt_time_nanos(void) {
    return XR_FROM_INT((int64_t) xrt_platform_monotonic_ns());
}

static inline XrValue xrt_time_micros(void) {
    return XR_FROM_INT((int64_t) (xrt_platform_monotonic_ns() / 1000ULL));
}

static inline XrValue xrt_time_clock(void) {
    return XR_FROM_INT((int64_t) (xrt_platform_process_cpu_ns() / 1000000ULL));
}

static inline XrValue xrt_time_local_offset(void) {
    return XR_FROM_INT((int64_t) xr_time_utc_offset_at(time(NULL)));
}

static inline XrValue xrt_time_local_offset_at(XrValue timestamp) {
    int64_t ts = XR_IS_INT(timestamp) ? XR_TO_INT(timestamp) : 0;
    return XR_FROM_INT((int64_t) xr_time_utc_offset_at((time_t) ts));
}

#endif

#endif  // XRT_TIME_H
