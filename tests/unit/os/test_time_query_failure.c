/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_time_query_failure.c - Failed POSIX queries never publish poisoned values
 */

#include "os/os_time.h"

#include <stdio.h>
#include <time.h>

static int query_status;
static clock_t cpu_ticks;

static int failed_clock_query(clockid_t clock_id, struct timespec *output) {
    (void) clock_id;
    /* A failed syscall may leave arbitrary output bytes. */
    output->tv_sec = 123;
    output->tv_nsec = 456;
    return query_status;
}

static clock_t failed_cpu_query(void) {
    return cpu_ticks;
}

#define clock_gettime failed_clock_query
#define clock failed_cpu_query
#include "os/unix/time_unix.c"
#undef clock
#undef clock_gettime

int main(void) {
    query_status = -1;
    cpu_ticks = (clock_t) -1;
    if (xr_time_monotonic_ns() != 0u || xr_time_realtime_ns() != 0u ||
        xr_time_process_cpu_ns() != 0u) {
        fputs("Failed clock query published a value\n", stderr);
        return 1;
    }
    query_status = 0;
    if (xr_time_monotonic_ns() != UINT64_C(123000000456) ||
        xr_time_realtime_ns() != UINT64_C(123000000456)) {
        fputs("Successful clock query changed its value\n", stderr);
        return 1;
    }
    puts("Clock query failures do not publish poisoned values");
    return 0;
}
