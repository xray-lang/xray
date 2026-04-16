/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * gc.h - GC monitoring module header
 *
 * KEY CONCEPT:
 *   Provides GC status query, memory statistics, and info functions.
 *   Per-coroutine GC is automatic, manual control functions are no-ops.
 */

#ifndef XR_STDLIB_GC_H
#define XR_STDLIB_GC_H

#include "../../src/runtime/xisolate_internal.h"

// Load gc module, registers for 'import gc'
struct XrModule* xr_load_module_gc(XrayIsolate *isolate);

#endif
