/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xresource_limits.h - Product and explicit sandbox resource policies
 *
 * KEY CONCEPT:
 *   Ordinary execution has no policy quota. Representability, allocation
 *   failure, ownership checks and cancellation still apply. Zero continues
 *   to grant no resources; maximum integers preserve checked accounting.
 */
#ifndef XRESOURCE_LIMITS_H
#define XRESOURCE_LIMITS_H
#include "xdefs.h"

#define XR_RESOURCE_DEFAULT_LIMIT UINT64_MAX
#define XR_RESOURCE_DEFAULT_DEPTH UINT32_MAX
#define XR_RESOURCE_DEFAULT_SIZE_LIMIT SIZE_MAX
#define XR_RESOURCE_DEFAULT_COMPILER_LIMITS {UINT64_MAX, UINT64_MAX, UINT64_MAX}

/* Sandboxed source admission and execution have independent cumulative
 * ledgers. These quotas do not grant filesystem or process isolation. */
#define XR_RESOURCE_SANDBOX_COMPILER_LIMITS \
    {UINT64_C(1) << 30, UINT64_C(256) << 20, UINT64_C(8) << 30}
#define XR_RESOURCE_SANDBOX_RUNTIME_LIVE (UINT64_C(16) << 20)
#define XR_RESOURCE_SANDBOX_RUNTIME_REQUESTED (UINT64_C(64) << 20)
#define XR_RESOURCE_SANDBOX_RUNTIME_WORK UINT64_C(128000000)
#define XR_RESOURCE_SANDBOX_RUNTIME_POLLS UINT64_C(1000000)
#define XR_RESOURCE_SANDBOX_RUNTIME_DEPTH UINT32_C(4096)

/* Interactive editor requests and preference-file operations retain their
 * existing finite policy until their owning tool explicitly changes it. */
#define XR_RESOURCE_TOOL_COMPILER_LIMITS \
    {UINT64_C(64) << 20, UINT64_C(8) << 20, XR_RESOURCE_SANDBOX_RUNTIME_WORK}
#define XR_RESOURCE_DEBUG_LIVE (UINT64_C(1) << 20)
#define XR_RESOURCE_DEBUG_POLLS UINT64_C(10000)
#define XR_RESOURCE_DEBUG_DEPTH UINT32_C(64)

#endif // XRESOURCE_LIMITS_H
