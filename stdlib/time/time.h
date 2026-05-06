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
 *   Provides wall-clock timestamps (now, milliseconds), monotonic
 *   timing (monotonic/nanos/micros), process CPU time (clock), and
 *   coroutine-friendly sleep.
 *
 * USAGE:
 *   import time
 *   let now = time.now()       // epoch milliseconds (wall clock)
 *   let t   = time.nanos()     // monotonic nanoseconds
 *   time.sleep(1500)           // yield for 1500 ms
 */

#ifndef XR_STDLIB_TIME_H
#define XR_STDLIB_TIME_H

#include "../../src/base/xdefs.h"

struct XrayIsolate;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_time(struct XrayIsolate *isolate);

#endif  // XR_STDLIB_TIME_H
