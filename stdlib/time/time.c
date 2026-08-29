/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * time.c - Time module implementation
 */

#include "../common.h"
#include "../../src/coro/xworker.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/vm/xvm.h"  // xr_yieldable_cfunction_new
#include "../../src/base/xchecks.h"
#include "../../src/os/os_time.h"
#include "../../src/shared/xr_time_offset.h"
#include <time.h>

// ========== Module-private native leaves ==========
//
// Each leaf answers the finest unit its clock reports and applies no policy.
// Unit scaling and the choice of clock behind a public reading belong to
// time.xr, which is the single place both backends compile them from.

// time.__realtimeNanos() -> int (nanoseconds since the Unix epoch)
static XrValue time_realtimeNanos(XrVMRuntime *isolate, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int((int64_t) xr_time_realtime_ns());
}

// time.__monotonicNanos() -> int (nanoseconds on the runtime's monotonic clock)
static XrValue time_monotonicNanos(XrVMRuntime *isolate, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int((int64_t) xr_runtime_current_monotonic_ns());
}

// time.__cpuNanos() -> int (nanoseconds of process CPU time)
static XrValue time_cpuNanos(XrVMRuntime *isolate, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int((int64_t) xr_time_process_cpu_ns());
}

// time.__utcOffsetAt(seconds: int) -> int (minutes east of UTC at a Unix time)
static XrValue time_utcOffsetAt(XrVMRuntime *isolate, XrValue *args, int nargs) {
    (void) isolate;
    int64_t ts = 0;
    if (nargs > 0 && XR_IS_INT(args[0]))
        ts = XR_TO_INT(args[0]);
    return xr_int((int64_t) xr_time_utc_offset_at((time_t) ts));
}

/*
 * Continuation for time.sleep — the timer has fired, just return null.
 */
static XrCFuncResult time_sleep_done(XrVMRuntime *X, int status, XrValue resume_value, void *ctx,
                                     XrValue *result) {
    (void) X;
    (void) status;
    (void) resume_value;
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
static XrCFuncResult xr_time_sleep(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result) {
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

#define XR_STDLIB_VM_BIND_MODULE_TIME 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_TIME
