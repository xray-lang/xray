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
#include "../../include/xray_platform.h"
#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"
#include "../../src/vm/xvm_internal.h"  // XrScheduler, XrCoroutine
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
    // Windows: QueryPerformanceCounter for nanosecond precision
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (int64_t)(counter.QuadPart * 1000000000LL / freq.QuadPart);
#elif defined(XR_PLATFORM_MACOS)
    return (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

// ========== Exported function implementations ==========

// time.now() -> int (milliseconds)
XrValue xr_time_now(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    (void)nargs;  // interface consistency, unused
    (void)args; 
    
    int64_t timestamp_ms = get_timestamp_ms();
    return xr_int(timestamp_ms);
}

// time.clock() -> int (milliseconds)
XrValue xr_time_clock(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    (void)nargs;  // interface consistency, unused
    (void)args; 
    
    clock_t t = clock();
    int64_t milliseconds = (int64_t)t * 1000 / CLOCKS_PER_SEC;
    return xr_int(milliseconds);
}

// time.monotonic() -> int (milliseconds)
XrValue xr_time_monotonic(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)nargs; (void)isolate; (void)args;
    return xr_int(get_monotonic_ns() / 1000000);
}

// time.nanos() -> int (nanoseconds, monotonic)
XrValue xr_time_nanos(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)nargs; (void)isolate; (void)args;
    return xr_int(get_monotonic_ns());
}

// time.micros() -> int (microseconds, monotonic)
XrValue xr_time_micros(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)nargs; (void)isolate; (void)args;
    return xr_int(get_monotonic_ns() / 1000);
}

/*
 * time.sleep(milliseconds: int) -> null
 *
 * Note: Compiler translates time.sleep() to OP_SLEEP instruction.
 * This function serves as fallback (for dynamic calls, etc.)
 */
XrValue xr_time_sleep(XrayIsolate *isolate, XrValue *args, int nargs) {
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
    
    // 1. Create native module
    XrModule *module = xr_module_create_native(isolate, "time");
    
    
    // 2. Add exported functions
    extern XrCFunction* xr_vm_cfunction_new(XrayIsolate *isolate, XrCFunctionPtr func, const char *name);
    extern XrValue xr_value_from_cfunction(XrCFunction *cfunc);
    
    // Helper macro: export C function
    #define EXPORT_CFUNC(name_str, func_ptr) \
        do { \
            XrCFunction *cfunc = xr_vm_cfunction_new(isolate, func_ptr, name_str); \
            XrValue fn_val = xr_value_from_cfunction(cfunc); \
            xr_module_add_export(isolate, module, name_str, fn_val); \
        } while(0)
    
    EXPORT_CFUNC("now", xr_time_now);
    EXPORT_CFUNC("clock", xr_time_clock);
    EXPORT_CFUNC("monotonic", xr_time_monotonic);
    EXPORT_CFUNC("nanos", xr_time_nanos);
    EXPORT_CFUNC("micros", xr_time_micros);
    EXPORT_CFUNC("sleep", xr_time_sleep);
    
    #undef EXPORT_CFUNC
    
    // 3. Mark as loaded
    module->loaded = true;
    
    
    return module;
}

