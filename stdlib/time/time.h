/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * time.h - Time module for xray standard library
 *
 * KEY CONCEPT:
 *   Provides time-related functions: timestamps, high-precision timing,
 *   and sleep control. All time units are in milliseconds.
 *
 * USAGE:
 *   import time
 *   let now = time.now()    // millisecond timestamp
 *   time.sleep(1500)        // sleep 1500 milliseconds
 */

#ifndef XR_STDLIB_TIME_H
#define XR_STDLIB_TIME_H

#include "../../src/runtime/value/xvalue.h"

// Forward declarations
struct VM;
struct XrModule;

// ========== Module exported functions ==========

/*
 * time.now() -> int
 *
 * Returns current Unix timestamp in milliseconds (integer)
 * e.g.: 1698403200123
 */
XrValue xr_time_now(XrayIsolate *isolate, XrValue *args, int nargs);

/*
 * time.clock() -> int
 *
 * Returns CPU clock time in milliseconds
 * Used for performance testing
 */
XrValue xr_time_clock(XrayIsolate *isolate, XrValue *args, int nargs);

/*
 * time.monotonic() -> int
 *
 * Returns monotonically increasing time in milliseconds
 * Not affected by system time adjustments
 * Best suited for timing and benchmarks
 */
XrValue xr_time_monotonic(XrayIsolate *isolate, XrValue *args, int nargs);

/*
 * time.nanos() -> int
 *
 * Returns monotonic time in nanoseconds
 * Highest precision timer available
 */
XrValue xr_time_nanos(XrayIsolate *isolate, XrValue *args, int nargs);

/*
 * time.micros() -> int
 *
 * Returns monotonic time in microseconds
 */
XrValue xr_time_micros(XrayIsolate *isolate, XrValue *args, int nargs);

/*
 * time.sleep(milliseconds: int) -> null
 *
 * Sleep for specified milliseconds
 *
 * @param milliseconds  sleep duration (must be >= 0)
 */
XrValue xr_time_sleep(XrayIsolate *isolate, XrValue *args, int nargs);


// ========== Module loader ==========

/*
 * Native module loader
 * Called by module system to create and initialize time module
 *
 * @param isolate  Isolate instance
 * @return         time module object
 */
struct XrModule* xr_load_module_time(XrayIsolate *isolate);

#endif

