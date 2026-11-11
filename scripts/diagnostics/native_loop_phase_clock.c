/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_loop_phase_clock.c - Nested diagnostic CPU accounting
 */
#include "native_loop_phase_clock.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

typedef struct Phase {
    uint64_t start, cpu;
    unsigned begins, ends, active;
} Phase;
static Phase phases[3];

static uint64_t process_cpu(void) {
#if defined(XR_OS_WINDOWS)
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) abort();
    uint64_t k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    uint64_t u = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
    return (k + u) * UINT64_C(100);
#else
    struct timespec value;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value)) abort();
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
#endif
}

XR_FUNC void phase_reset(void) { memset(phases, 0, sizeof phases); }
XR_FUNC void phase_begin(unsigned phase) {
    if (phase >= 3 || phases[phase].active) abort();
    phases[phase].active = 1;
    phases[phase].begins++;
    phases[phase].start = process_cpu();
}
XR_FUNC void phase_end(unsigned phase) {
    uint64_t end = process_cpu();
    if (phase >= 3 || !phases[phase].active || end < phases[phase].start) abort();
    phases[phase].cpu += end - phases[phase].start;
    phases[phase].active = 0;
    phases[phase].ends++;
}
XR_FUNC void phase_report(const char *engine, int completed) {
    uint64_t clock_min = UINT64_MAX;
    for (unsigned i = 0; i < 32; ++i) {
        uint64_t first = process_cpu(), second = process_cpu();
        if (second < first) abort();
        if (second - first < clock_min) clock_min = second - first;
    }
    const char *clock_name =
#if defined(XR_OS_WINDOWS)
        "GetProcessTimes";
#else
        "CLOCK_PROCESS_CPUTIME_ID";
#endif
    fprintf(stderr, "XR_PHASE {\"schema\":1,\"engine\":\"%s\",\"clock\":\"%s\",\"completed\":%d,"
        "\"cpu_ns\":[%llu,%llu,%llu],\"begins\":[%u,%u,%u],"
        "\"ends\":[%u,%u,%u],\"active\":[%u,%u,%u],\"clock_pair_min_ns\":%llu}\n",
        engine, clock_name, completed,
        (unsigned long long)phases[0].cpu, (unsigned long long)phases[1].cpu,
        (unsigned long long)phases[2].cpu,
        phases[0].begins, phases[1].begins, phases[2].begins,
        phases[0].ends, phases[1].ends, phases[2].ends,
        phases[0].active, phases[1].active, phases[2].active,
        (unsigned long long)clock_min);
}
