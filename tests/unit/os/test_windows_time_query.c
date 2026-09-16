/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_windows_time_query.c - Execute Windows clock logic with injected API results
 */

#include "base/xdefs.h"
#include <windows.h>
#include <stdio.h>

/* Load the host SDK before redirecting only the clock APIs. The portable
 * declarations must never shadow Windows headers used by the platform layer. */
#define QueryPerformanceFrequency xr_test_QueryPerformanceFrequency
#define QueryPerformanceCounter xr_test_QueryPerformanceCounter
#define GetSystemTimePreciseAsFileTime xr_test_GetSystemTimePreciseAsFileTime
#define GetCurrentProcess xr_test_GetCurrentProcess
#define GetProcessTimes xr_test_GetProcessTimes
#define Sleep xr_test_Sleep

static BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency);
static BOOL QueryPerformanceCounter(LARGE_INTEGER *counter);
static void GetSystemTimePreciseAsFileTime(FILETIME *value);
static void *GetCurrentProcess(void);
static BOOL GetProcessTimes(void *process, FILETIME *creation, FILETIME *exit, FILETIME *kernel,
                            FILETIME *user);
static void Sleep(DWORD milliseconds);

#include "os/win/time_win.c"

static BOOL frequency_ok = 1;
static BOOL counter_ok = 1;
static int64_t frequency_ticks = 10000000;
static int64_t counter_ticks = 15000000;
static uint64_t sleep_milliseconds;
static unsigned infinite_sleeps;

static BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency) {
    frequency->QuadPart = frequency_ticks;
    return frequency_ok;
}

static BOOL QueryPerformanceCounter(LARGE_INTEGER *counter) {
    counter->QuadPart = counter_ticks;
    return counter_ok;
}

static void GetSystemTimePreciseAsFileTime(FILETIME *value) {
    *value = (FILETIME) {0};
}

static void *GetCurrentProcess(void) {
    return NULL;
}

static BOOL GetProcessTimes(void *process, FILETIME *creation, FILETIME *exit, FILETIME *kernel,
                            FILETIME *user) {
    (void) process;
    (void) creation;
    (void) exit;
    (void) kernel;
    (void) user;
    return 0;
}

static void Sleep(DWORD milliseconds) {
    sleep_milliseconds += milliseconds;
    infinite_sleeps += milliseconds == MAXDWORD;
}

static int queries(void) {
    if (xr_time_monotonic_ns() != UINT64_C(1500000000))
        return 1;
    counter_ok = 0;
    if (xr_time_monotonic_ns() != 0u)
        return 2;
    counter_ok = 1;
    frequency_ok = 0;
    if (xr_time_monotonic_ns() != 0u)
        return 3;
    frequency_ok = 1;
    frequency_ticks = 0;
    if (xr_time_monotonic_ns() != 0u)
        return 4;
    frequency_ticks = -1;
    if (xr_time_monotonic_ns() != 0u)
        return 5;
    frequency_ticks = 20000000;
    counter_ok = 0;
    if (xr_time_monotonic_ns() != 0u)
        return 6;
    counter_ok = 1;
    counter_ticks = -1;
    if (xr_time_monotonic_ns() != 0u || xr_time_process_cpu_ns() != 0u)
        return 7;
    frequency_ticks = INT64_MAX;
    counter_ticks = INT64_MAX - 1;
    if (xr_time_monotonic_ns() != UINT64_C(999999999))
        return 11;
    frequency_ticks = 3;
    counter_ticks = 1;
    if (xr_time_monotonic_ns() != UINT64_C(333333333))
        return 12;
    return 0;
}

static int scaled_queries(void) {
    static const struct {
        int64_t frequency;
        int64_t counter;
        uint64_t nanos;
    } cases[] = {
        {INT64_C(2439707488267304297), INT64_C(8752888668149373689), UINT64_C(3587679551)},
        {INT64_C(8163219068346088635), INT64_C(5174512264569493161), UINT64_C(633881342)},
        {INT64_C(396649273063946970), INT64_C(7884641629312824856), UINT64_C(19878119448)},
        {INT64_C(6585288324445169658), INT64_C(1742936556546173130), UINT64_C(264671259)},
        {INT64_C(1413458475988416064), INT64_C(4764138191649587767), UINT64_C(3370554050)},
        {INT64_C(8504021200379666185), INT64_C(1225462887419837338), UINT64_C(144103931)},
        {INT64_C(2101041576386790883), INT64_C(6869758959390016826), UINT64_C(3269692059)},
        {INT64_C(4122418785801493044), INT64_C(8507244260601520060), UINT64_C(2063653573)},
        {INT64_C(8632145220389635709), INT64_C(660869288940024172), UINT64_C(76559102)},
        {INT64_C(6931897403757867342), INT64_C(495703323133506681), UINT64_C(71510481)},
        {INT64_C(5277081234883197649), INT64_C(8608017014642657349), UINT64_C(1631207978)},
        {INT64_C(1191314621905517537), INT64_C(5484921603948850906), UINT64_C(4604091566)},
        {INT64_C(7650963587845937651), INT64_C(3809000168980855533), UINT64_C(497845810)},
        {INT64_C(457875537410354607), INT64_C(1056611620178735689), UINT64_C(2307639377)},
        {INT64_C(1118288839478674071), INT64_C(265642202136285568), UINT64_C(237543461)},
        {INT64_C(5142022747054357600), INT64_C(8979761811135451326), UINT64_C(1746348130)},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        frequency_ticks = cases[i].frequency;
        counter_ticks = cases[i].counter;
        if (xr_time_monotonic_ns() != cases[i].nanos)
            return 13;
    }
    return 0;
}

int main(void) {
    int failure = queries();
    if (!failure)
        failure = scaled_queries();
    if (failure) {
        fprintf(stderr, "Clock query scenario %d failed\n", failure);
        return failure;
    }
    xr_time_sleep_ns(0u);
    if (sleep_milliseconds != 0u)
        return 8;
    xr_time_sleep_ns(1u);
    if (sleep_milliseconds != 1u)
        return 9;
    sleep_milliseconds = 0u;
    xr_time_sleep_ns(UINT64_MAX);
    if (sleep_milliseconds != UINT64_C(18446744073710) || infinite_sleeps != 0u)
        return 10;
    puts("Windows clock queries and finite sleeps preserve API failure boundaries");
    return 0;
}
