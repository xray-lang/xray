/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * runtime.h - Xray runtime introspection/control module header
 *
 * KEY CONCEPT:
 *   Control plane for the Xray runtime itself: cycle-collector control and
 *   per-coroutine memory statistics (task 154 moved these out of `mem`,
 *   which now only carries raw-memory capabilities). Xray reclamation is
 *   per-coroutine reference counting + Bacon-Rajan cycle collection — not a
 *   tracing GC — hence `runtime.*`, not `gc.*`.
 */

#ifndef XR_STDLIB_RUNTIME_H
#define XR_STDLIB_RUNTIME_H

#include "../../src/base/xdefs.h"

struct XrVMRuntime;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_runtime(struct XrVMRuntime *isolate);

#endif  // XR_STDLIB_RUNTIME_H
