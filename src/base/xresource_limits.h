/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xresource_limits.h - Product and explicit sandbox resource policies
 *
 * KEY CONCEPT:
 *   Compiler memory admission is explicit and independent of execution fuel.
 *   Ordinary compilation has no finite quota or cumulative turnover limit.
 */
#ifndef XRESOURCE_LIMITS_H
#define XRESOURCE_LIMITS_H
#include "xcompile_resources.h"

#define XR_RESOURCE_DEFAULT_LIMIT UINT64_MAX
#define XR_RESOURCE_DEFAULT_DEPTH UINT32_MAX
#define XR_RESOURCE_DEFAULT_SIZE_LIMIT SIZE_MAX
#define XR_RESOURCE_DEFAULT_COMPILER_LIMITS {XR_COMPILE_MEMORY_ORDINARY, 0, false}

/* Finite compiler memory and runtime sandbox fuel have separate owners.
 * These limits do not grant filesystem or process isolation. */
#define XR_RESOURCE_SANDBOX_COMPILER_LIMITS \
    {XR_COMPILE_MEMORY_FINITE, UINT64_C(256) << 20, false}
#define XR_RESOURCE_SANDBOX_RUNTIME_LIVE (UINT64_C(16) << 20)
#define XR_RESOURCE_SANDBOX_RUNTIME_REQUESTED (UINT64_C(64) << 20)
#define XR_RESOURCE_SANDBOX_RUNTIME_WORK UINT64_C(128000000)
#define XR_RESOURCE_SANDBOX_RUNTIME_POLLS UINT64_C(1000000)
#define XR_RESOURCE_SANDBOX_RUNTIME_DEPTH UINT32_C(4096)

/* Editor requests use a finite held-memory policy without cumulative work. */
#define XR_RESOURCE_TOOL_COMPILER_LIMITS \
    {XR_COMPILE_MEMORY_FINITE, UINT64_C(8) << 20, false}
#define XR_RESOURCE_DEBUG_LIVE (UINT64_C(1) << 20)
#define XR_RESOURCE_DEBUG_POLLS UINT64_C(10000)
#define XR_RESOURCE_DEBUG_DEPTH UINT32_C(64)

#endif // XRESOURCE_LIMITS_H
