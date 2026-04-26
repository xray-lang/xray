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

struct XrModule;

/*
 * Native module loader.
 * The per-function C bindings (time.now, time.clock, etc.) are file-private
 * to time.c; callers reach them through the registered module export table.
 */
struct XrModule *xr_load_module_time(XrayIsolate *isolate);

#endif  // XR_STDLIB_TIME_H
