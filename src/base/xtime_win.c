/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtime_win.c - Windows implementation of xtime.h.
 *
 * - Monotonic clock: QueryPerformanceCounter is the canonical
 *   choice. The frequency is fetched once and cached; conversion
 *   to nanoseconds is done with 128-bit math via mul/div tricks
 *   to avoid overflow on long-running processes.
 * - Realtime clock: GetSystemTimePreciseAsFileTime gives 100ns
 *   resolution on Windows 8+. The 1601 → 1970 epoch shift is
 *   the standard 11644473600 seconds.
 * - Sleep: Sleep() is millisecond resolution. For sub-ms requests
 *   we round up to 1ms; the API contract is "at least N ns",
 *   which over-sleeping respects.
 */

#include "xtime.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Difference between Windows FILETIME epoch (1601-01-01) and
// Unix epoch (1970-01-01), expressed in 100ns ticks.
#define XR_FILETIME_TO_UNIX_100NS 116444736000000000ULL

static LARGE_INTEGER xr_qpc_freq__;

static void xr_qpc_init__(void) {
    if (xr_qpc_freq__.QuadPart == 0) {
        QueryPerformanceFrequency(&xr_qpc_freq__);
    }
}

uint64_t xr_time_monotonic_ns(void) {
    xr_qpc_init__();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    // Convert ticks to nanoseconds without losing precision:
    //   ns = counter * 1e9 / freq
    // Split counter into seconds and remainder so the multiply
    // by 1e9 cannot overflow on multi-day uptimes.
    uint64_t freq = (uint64_t) xr_qpc_freq__.QuadPart;
    uint64_t whole_seconds = (uint64_t) counter.QuadPart / freq;
    uint64_t remainder = (uint64_t) counter.QuadPart % freq;
    return whole_seconds * 1000000000ULL + (remainder * 1000000000ULL) / freq;
}

uint64_t xr_time_realtime_ns(void) {
    FILETIME ft;
    // GetSystemTimePreciseAsFileTime is Windows 8+ and gives
    // sub-microsecond resolution. We require Windows 10 anyway.
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // FILETIME counts 100ns ticks since 1601; rebase to Unix and
    // scale to ns.
    return (u.QuadPart - XR_FILETIME_TO_UNIX_100NS) * 100ULL;
}

void xr_time_sleep_ns(uint64_t ns) {
    // Sleep() resolution is 1ms. Round up: the contract is
    // "at least ns nanoseconds", so over-sleeping is allowed but
    // under-sleeping is not.
    uint64_t ms = (ns + 999999ULL) / 1000000ULL;
    while (ms > 0) {
        DWORD chunk = (ms > MAXDWORD) ? MAXDWORD : (DWORD) ms;
        Sleep(chunk);
        ms -= chunk;
    }
}

#endif  // _WIN32
