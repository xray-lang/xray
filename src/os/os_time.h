/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtime.h - Cross-platform monotonic and real-time clocks plus sleep.
 *
 * KEY CONCEPT:
 *   Single nanosecond-precision API used everywhere instead of
 *   raw POSIX clock_gettime / nanosleep / usleep / gettimeofday.
 *
 *   - xr_time_monotonic_ns(): strictly increasing wall-independent
 *     timer. Use for elapsed-time math, timeouts, deadlines.
 *   - xr_time_realtime_ns():  Unix-epoch wall clock. Use only for
 *     human-facing timestamps; never for elapsed math (NTP can
 *     jump it backwards).
 *   - xr_time_sleep_ns():     best-effort kernel sleep. Returns
 *     when the deadline passes; the kernel may oversleep.
 *
 *   The implementation is split into xtime_unix.c (clock_gettime,
 *   nanosleep) and xtime_win.c (QueryPerformanceCounter,
 *   GetSystemTimePreciseAsFileTime, Sleep). Each file's body is
 *   guarded so the GLOB-based build picks up exactly one TU per
 *   platform.
 *
 * RELATED MODULES:
 *   - base/xdefs.h: visibility/attribute macros
 */

#ifndef XTIME_H
#define XTIME_H

#include "../base/xdefs.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Nanoseconds since an arbitrary, process-stable epoch. Strictly
// monotonic. Suitable for measuring elapsed time and computing
// timeouts/deadlines.
XR_FUNC uint64_t xr_time_monotonic_ns(void);

// Nanoseconds since the Unix epoch (1970-01-01 UTC). Wall clock;
// may jump forward or backward when the system clock is adjusted.
XR_FUNC uint64_t xr_time_realtime_ns(void);

// Nanoseconds of CPU time consumed by the current process (user +
// kernel). Returns 0 if the platform does not support this query.
// Use for profiling; not meaningful as a wall-clock substitute.
XR_FUNC uint64_t xr_time_process_cpu_ns(void);

// Sleep for at least `ns` nanoseconds. Best effort; the kernel
// may schedule the wake-up later under load.
XR_FUNC void xr_time_sleep_ns(uint64_t ns);

// Convenience: sleep for at least `ms` milliseconds.
static inline void xr_time_sleep_ms(uint64_t ms) {
    xr_time_sleep_ns(ms * 1000000ULL);
}

// Convenience: sleep for at least `us` microseconds.
static inline void xr_time_sleep_us(uint64_t us) {
    xr_time_sleep_ns(us * 1000ULL);
}

// Convenience: monotonic clock in milliseconds.
static inline uint64_t xr_time_monotonic_ms(void) {
    return xr_time_monotonic_ns() / 1000000ULL;
}

#ifdef __cplusplus
}
#endif

#endif  // XTIME_H
