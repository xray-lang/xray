/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * mem.h - Raw-memory capability module header
 *
 * KEY CONCEPT:
 *   Raw-memory capabilities: fence, cache hints, explicit allocation,
 *   anonymous pages, address bridge, bulk byte ops, volatile MMIO access.
 *   Runtime/GC introspection lives in stdlib/runtime (task 154).
 */

#ifndef XR_STDLIB_MEM_H
#define XR_STDLIB_MEM_H

#include "../../src/base/xdefs.h"

struct XrVMRuntime;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_mem(struct XrVMRuntime *isolate);

#endif  // XR_STDLIB_MEM_H
