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
#include "../../src/vm/xvm_internal.h"  // XrCoroState, XrCoroutine
#include "../../src/coro/xyieldable.h"  // xr_yield_for_timeout
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
    int64_t t = ((int64_t) ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000LL) / 10000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static int64_t get_monotonic_ns(void) {
#ifdef XR_PLATFORM_WINDOWS
    // Windows: QueryPerformanceCounter for nanosecond precision. Compute
    // seconds and the sub-second remainder separately so the intermediate
    // product never overflows int64 — the naive counter * 1e9 expression
    // wraps after a few days on a 10 MHz performance counter.
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    int64_t q = counter.QuadPart;
    int64_t f = freq.QuadPart;
    int64_t sec = q / f;
    int64_t sub_ns = ((q % f) * 1000000000LL) / f;
    return sec * 1000000000LL + sub_ns;
#elif defined(XR_PLATFORM_MACOS)
    return (int64_t) clock_gettime_nsec_np(CLOCK_MONOTONIC);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

// ========== Module-private function bindings ==========

// time.now() -> int (milliseconds)
static XrValue xr_time_now(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int(get_timestamp_ms());
}

// time.clock() -> int (milliseconds)
//
// POSIX `clock_t` is a 32-bit type at 1 MHz on Linux/macOS, so `clock()`
// overflows after ~70 minutes. Prefer CLOCK_PROCESS_CPUTIME_ID when
// available, which is monotonic across the full 64-bit range.
static XrValue xr_time_clock(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
#if defined(XR_PLATFORM_MACOS) || defined(XR_PLATFORM_LINUX)
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) {
        int64_t ms = (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        return xr_int(ms);
    }
#endif
    clock_t t = clock();
    int64_t milliseconds = (int64_t) t * 1000 / CLOCKS_PER_SEC;
    return xr_int(milliseconds);
}

// time.monotonic() -> int (milliseconds)
static XrValue xr_time_monotonic(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int(get_monotonic_ns() / 1000000);
}

// time.nanos() -> int (nanoseconds, monotonic)
static XrValue xr_time_nanos(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int(get_monotonic_ns());
}

// time.micros() -> int (microseconds, monotonic)
static XrValue xr_time_micros(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int(get_monotonic_ns() / 1000);
}

/*
 * Continuation for time.sleep — the timer has fired, just return null.
 */
static XrCFuncResult time_sleep_done(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    (void) X;
    (void) status;
    (void) ctx;
    *result = xr_null();
    return XR_CFUNC_DONE;
}

/*
 * time.sleep(milliseconds: int) -> null
 *
 * Coroutine-friendly: yields via xr_yield_for_timeout so the worker
 * thread is free to run other coroutines during the sleep.
 *
 * Note: the compiler normally translates time.sleep() to OP_SLEEP.
 * This yieldable C function is the dynamic-dispatch fallback that
 * was previously a blocking nanosleep.
 */
static XrCFuncResult xr_time_sleep(XrayIsolate *X, XrValue *args, int nargs, XrValue *result) {
    if (nargs < 1 || (!XR_IS_INT(args[0]) && !XR_IS_FLOAT(args[0]))) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    int64_t ms = XR_IS_INT(args[0]) ? XR_TO_INT(args[0]) : (int64_t) XR_TO_FLOAT(args[0]);
    if (ms <= 0) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    return xr_yield_for_timeout(X, ms, time_sleep_done, NULL, result);
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

XrModule *xr_load_module_time(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_time: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "time");
    if (!module)
        return NULL;

    // 2. Add exported functions
    XRS_EXPORT(module, isolate, "now", xr_time_now);
    XRS_EXPORT(module, isolate, "clock", xr_time_clock);
    XRS_EXPORT(module, isolate, "monotonic", xr_time_monotonic);
    XRS_EXPORT(module, isolate, "nanos", xr_time_nanos);
    XRS_EXPORT(module, isolate, "micros", xr_time_micros);
    XRS_EXPORT_YIELDABLE(module, isolate, "sleep", xr_time_sleep);

    // 3. Mark as loaded
    module->loaded = true;
    return module;
}
