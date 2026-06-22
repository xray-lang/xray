/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * mem.h - RC memory and cycle-collection introspection module header
 *
 * KEY CONCEPT:
 *   Per-coroutine memory and cycle-collection statistics. All operations
 *   target the current coroutine's heap, not a global tracing collector.
 */

#ifndef XR_STDLIB_MEM_H
#define XR_STDLIB_MEM_H

#include "../../src/base/xdefs.h"

struct XrayIsolate;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_mem(struct XrayIsolate *isolate);

#endif  // XR_STDLIB_MEM_H
