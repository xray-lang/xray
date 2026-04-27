/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtime_unix.c - POSIX implementation of xtime.h.
 *
 * Uses clock_gettime(CLOCK_MONOTONIC / CLOCK_REALTIME) for time and
 * nanosleep for sleeping. CLOCK_MONOTONIC is the right choice for
 * deadlines: Linux glibc, macOS 10.12+, BSD all expose it and it
 * tracks an arbitrary process-stable origin.
 *
 * On macOS we deliberately use clock_gettime(CLOCK_MONOTONIC_RAW)
 * was avoided: the plain CLOCK_MONOTONIC is sufficient and matches
 * the Linux semantics our coroutine scheduler assumes.
 */

#include "../os_time.h"

#ifndef _WIN32

#include <errno.h>
#include <time.h>

uint64_t xr_time_monotonic_ns(void) {
    struct timespec ts;
    // CLOCK_MONOTONIC: never goes backwards, frozen across suspend
    // on most kernels. Adequate for all our timing needs.
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

uint64_t xr_time_realtime_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

void xr_time_sleep_ns(uint64_t ns) {
    struct timespec req;
    req.tv_sec = (time_t) (ns / 1000000000ULL);
    req.tv_nsec = (long) (ns % 1000000000ULL);
    // Loop on EINTR so signal interruption does not shorten the
    // requested wait.
    struct timespec rem;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        req = rem;
    }
}

#endif  // !_WIN32
