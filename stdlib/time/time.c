/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * time.c - Time module implementation
 */

#include "time.h"
#include "../common.h"
#include "../../include/xray_platform.h"
#include "../../src/vm/xvm_internal.h"  // XrScheduler, XrCoroutine
#include "../../src/base/xchecks.h"
#include <time.h>
#include <stdio.h>

#ifndef XR_PLATFORM_WINDOWS
#include <sys/time.h>
#include <unistd.h>
#endif

// ========== Helper functions ==========

static int64_t get_timestamp_ms(void) {
#ifdef XR_PLATFORM_WINDOWS
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int64_t t = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000LL) / 10000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static int64_t get_monotonic_ns(void) {
#ifdef XR_PLATFORM_WINDOWS
    // Windows: QueryPerformanceCounter for nanosecond precision. Compute
    // seconds and the sub-second remainder separately so the intermediate
    // product never overflows int64 — the naive counter * 1e9 expression
    // wraps after a few days on a 10 MHz performance counter.
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    int64_t q = counter.QuadPart;
    int64_t f = freq.QuadPart;
    int64_t sec     = q / f;
    int64_t sub_ns  = ((q % f) * 1000000000LL) / f;
    return sec * 1000000000LL + sub_ns;
#elif defined(XR_PLATFORM_MACOS)
    return (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

// ========== Module-private function bindings ==========

// time.now() -> int (milliseconds)
static XrValue xr_time_now(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate; (void)args; (void)nargs;
    return xr_int(get_timestamp_ms());
}

// time.clock() -> int (milliseconds)
//
// POSIX `clock_t` is a 32-bit type at 1 MHz on Linux/macOS, so `clock()`
// overflows after ~70 minutes. Prefer CLOCK_PROCESS_CPUTIME_ID when
// available, which is monotonic across the full 64-bit range.
static XrValue xr_time_clock(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate; (void)args; (void)nargs;
#if defined(XR_PLATFORM_MACOS) || defined(XR_PLATFORM_LINUX)
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) {
        int64_t ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        return xr_int(ms);
    }
#endif
    clock_t t = clock();
    int64_t milliseconds = (int64_t)t * 1000 / CLOCKS_PER_SEC;
    return xr_int(milliseconds);
}

// time.monotonic() -> int (milliseconds)
static XrValue xr_time_monotonic(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate; (void)args; (void)nargs;
    return xr_int(get_monotonic_ns() / 1000000);
}

// time.nanos() -> int (nanoseconds, monotonic)
static XrValue xr_time_nanos(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate; (void)args; (void)nargs;
    return xr_int(get_monotonic_ns());
}

// time.micros() -> int (microseconds, monotonic)
static XrValue xr_time_micros(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate; (void)args; (void)nargs;
    return xr_int(get_monotonic_ns() / 1000);
}

/*
 * time.sleep(milliseconds: int) -> null
 *
 * Note: the compiler normally translates time.sleep() to OP_SLEEP. This
 * function is the dynamic-dispatch fallback and remains a blocking call
 * until the coroutine-timer integration lands (see stdlib_basic_tools.md).
 */
static XrValue xr_time_sleep(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;

    if (nargs < 1) {
        fprintf(stderr, "time.sleep() requires 1 argument\n");
        return xr_null();
    }

    if (!XR_IS_FLOAT(args[0]) && !XR_IS_INT(args[0])) {
        fprintf(stderr, "time.sleep() argument must be a number\n");
        return xr_null();
    }

    int64_t milliseconds = XR_IS_INT(args[0]) ? XR_TO_INT(args[0]) : (int64_t)XR_TO_FLOAT(args[0]);
    if (milliseconds < 0) {
        fprintf(stderr, "time.sleep() argument must be non-negative\n");
        return xr_null();
    }

    // Blocking sleep (fallback implementation) - use platform abstraction
    xr_sleep_ms((uint32_t)milliseconds);
    return xr_null();
}


// ========== Module loader ==========

/*
 * Native module loader
 * Loads time module
 * Returns: Module object
 */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module time

XR_DEFINE_BUILTIN(xr_time_now, "now", "(): int", "Current time in milliseconds since epoch")
XR_DEFINE_BUILTIN(xr_time_clock, "clock", "(): int", "CPU clock time in milliseconds")
XR_DEFINE_BUILTIN(xr_time_monotonic, "monotonic", "(): int", "Monotonic time in milliseconds")
XR_DEFINE_BUILTIN(xr_time_nanos, "nanos", "(): int", "Monotonic time in nanoseconds")
XR_DEFINE_BUILTIN(xr_time_micros, "micros", "(): int", "Monotonic time in microseconds")
XR_DEFINE_BUILTIN(xr_time_sleep, "sleep", "(ms: int): void", "Sleep for milliseconds")

XrModule* xr_load_module_time(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_time: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "time");
    if (!module) return NULL;

    // 2. Add exported functions
    XRS_EXPORT(module, isolate, "now", xr_time_now);
    XRS_EXPORT(module, isolate, "clock", xr_time_clock);
    XRS_EXPORT(module, isolate, "monotonic", xr_time_monotonic);
    XRS_EXPORT(module, isolate, "nanos", xr_time_nanos);
    XRS_EXPORT(module, isolate, "micros", xr_time_micros);
    XRS_EXPORT(module, isolate, "sleep", xr_time_sleep);

    // 3. Mark as loaded
    module->loaded = true;
    return module;
}

