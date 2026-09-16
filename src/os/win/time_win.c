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
 *   choice. Query state belongs to the call, so simultaneous first
 *   queries never initialize shared mutable storage.
 * - Realtime clock: GetSystemTimePreciseAsFileTime gives 100ns
 *   resolution on Windows 8+. The 1601 → 1970 epoch shift is
 *   the standard 11644473600 seconds.
 * - Sleep: Sleep() is millisecond resolution. For sub-ms requests
 *   we round up to 1ms; the API contract is "at least N ns",
 *   which over-sleeping respects.
 */

#include "../os_time.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Difference between Windows FILETIME epoch (1601-01-01) and
// Unix epoch (1970-01-01), expressed in 100ns ticks.
#define XR_FILETIME_TO_UNIX_100NS 116444736000000000ULL

static uint64_t xr_time_fractional_ns__(uint64_t remainder, uint64_t frequency) {
    const uint64_t scale = UINT64_C(1000000000);
    if (remainder <= UINT64_MAX / scale)
        return remainder * scale / frequency;
    /* Long multiplication with a reduced residue keeps every intermediate
     * bounded, including frequencies near the signed host counter limit. */
    uint64_t nanos = 0;
    uint64_t residue = 0;
    for (uint32_t bit = UINT32_C(1) << 29; bit != 0; bit >>= 1) {
        nanos *= 2;
        if (residue >= frequency - residue) {
            residue -= frequency - residue;
            ++nanos;
        } else {
            residue *= 2;
        }
        if ((scale & bit) != 0) {
            if (residue >= frequency - remainder) {
                residue -= frequency - remainder;
                ++nanos;
            } else {
                residue += remainder;
            }
        }
    }
    return nanos;
}

uint64_t xr_time_monotonic_ns(void) {
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter) || counter.QuadPart < 0)
        return 0;
    // Convert ticks to nanoseconds without losing precision:
    //   ns = counter * 1e9 / freq
    // Split counter into seconds and remainder so the multiply
    // by 1e9 cannot overflow on multi-day uptimes.
    uint64_t freq = (uint64_t) frequency.QuadPart;
    uint64_t whole_seconds = (uint64_t) counter.QuadPart / freq;
    uint64_t remainder = (uint64_t) counter.QuadPart % freq;
    return whole_seconds * 1000000000ULL + xr_time_fractional_ns__(remainder, freq);
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

uint64_t xr_time_process_cpu_ns(void) {
    FILETIME creation, exit, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        ULARGE_INTEGER uk, uu;
        uk.LowPart = kernel.dwLowDateTime;
        uk.HighPart = kernel.dwHighDateTime;
        uu.LowPart = user.dwLowDateTime;
        uu.HighPart = user.dwHighDateTime;
        /* FILETIME ticks are 100ns each. */
        return (uk.QuadPart + uu.QuadPart) * 100ULL;
    }
    return 0;
}

void xr_time_sleep_ns(uint64_t ns) {
    // Sleep() resolution is 1ms. Round up: the contract is
    // "at least ns nanoseconds", so over-sleeping is allowed but
    // under-sleeping is not.
    uint64_t ms = ns / 1000000ULL + (ns % 1000000ULL != 0);
    while (ms > 0) {
        // MAXDWORD is the infinite-wait sentinel, never a finite chunk.
        DWORD chunk = (ms >= MAXDWORD) ? MAXDWORD - 1u : (DWORD) ms;
        Sleep(chunk);
        ms -= chunk;
    }
}
