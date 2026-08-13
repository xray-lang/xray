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
#include "../../src/runtime/value/xvalue.h"

struct XrVMRuntime;
struct XrModule;
struct XrAggregateLayout;

XR_FUNC struct XrModule *xr_native_module_create_mem(struct XrVMRuntime *isolate);
XR_FUNC int64_t xr_mem_buffer_length(XrValue value);
XR_FUNC bool xr_mem_buffer_bytes(XrValue value, const uint8_t **data, size_t *length);
XR_FUNC XrValue xr_mem_buffer_copy_from_bytes(struct XrVMRuntime *isolate, const uint8_t *data,
                                              size_t length);
/* Consume the byte payload after the compiler has proved complete native
 * output. Aggregate padding is canonicalized to zero instead of being copied
 * from foreign memory. */
XR_FUNC bool xr_mem_buffer_materialize(XrValue value, void *dst, size_t size, size_t align,
                                       const struct XrAggregateLayout *layout);

#endif  // XR_STDLIB_MEM_H
