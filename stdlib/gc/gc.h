/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * gc.h - GC control and monitoring module header
 *
 * KEY CONCEPT:
 *   Per-coroutine GC control and statistics. All operations target
 *   the current coroutine's GC instance, not a global collector.
 */

#ifndef XR_STDLIB_GC_H
#define XR_STDLIB_GC_H

#include "../../src/base/xdefs.h"

struct XrayIsolate;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_gc(struct XrayIsolate *isolate);

#endif  // XR_STDLIB_GC_H
