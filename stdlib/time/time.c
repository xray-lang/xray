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
#include "../../src/coro/xyieldable.h"
#include "../../src/vm/xvm.h"  // xr_vm_yieldable_cfunction_new
#include "../../src/base/xchecks.h"
#include "../../src/os/os_time.h"

// ========== Helper functions ==========

static int64_t get_timestamp_ms(void) {
    return (int64_t) (xr_time_realtime_ns() / 1000000ULL);
}

static int64_t get_monotonic_ns(void) {
    return (int64_t) xr_time_monotonic_ns();
}

// ========== Module-private function bindings ==========

// time.now() -> int (milliseconds)
static XrValue xr_time_now(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int(get_timestamp_ms());
}

// time.clock() -> int (milliseconds of process CPU time)
static XrValue xr_time_clock(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int((int64_t) (xr_time_process_cpu_ns() / 1000000ULL));
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

    /* Cap at 24 hours to prevent timer-wheel overflow or scheduler
     * starvation from accidentally huge values. */
    static const int64_t MAX_SLEEP_MS = 24LL * 60 * 60 * 1000;
    if (ms > MAX_SLEEP_MS)
        ms = MAX_SLEEP_MS;

    return xr_yield_for_timeout(X, ms, time_sleep_done, NULL, result);
}

// ========== Module loader ==========

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module time

XR_DEFINE_BUILTIN(xr_time_now, "now", "(): int", "Current time in milliseconds since epoch")
XR_DEFINE_BUILTIN(xr_time_clock, "clock", "(): int", "CPU clock time in milliseconds")
XR_DEFINE_BUILTIN(xr_time_monotonic, "monotonic", "(): int", "Monotonic time in milliseconds")
XR_DEFINE_BUILTIN(xr_time_nanos, "nanos", "(): int", "Monotonic time in nanoseconds")
XR_DEFINE_BUILTIN(xr_time_micros, "micros", "(): int", "Monotonic time in microseconds")
XR_DEFINE_BUILTIN(xr_time_sleep, "sleep", "(ms: int): void", "Sleep for milliseconds")

XR_FUNC XrModule *xr_load_module_time(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_time: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "time");
    if (!module)
        return NULL;

    XRS_EXPORT(module, isolate, "now", xr_time_now);
    XRS_EXPORT(module, isolate, "clock", xr_time_clock);
    XRS_EXPORT(module, isolate, "monotonic", xr_time_monotonic);
    XRS_EXPORT(module, isolate, "nanos", xr_time_nanos);
    XRS_EXPORT(module, isolate, "micros", xr_time_micros);
    XRS_EXPORT_YIELDABLE(module, isolate, "sleep", xr_time_sleep);

    module->loaded = true;
    return module;
}
