/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_debug.h - Coroutine debug info pool
 *
 * KEY CONCEPT:
 *   Million-concurrency optimization: externalize debug info from coroutine struct.
 *   Coroutine only stores debug_idx index, lookup table for details.
 *   - Reduces coroutine struct size by ~24 bytes
 *   - Debug info globally shared, avoids duplicate storage
 */

#ifndef XCORO_DEBUG_H
#define XCORO_DEBUG_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../base/xthread.h"
#include "../base/xdefs.h"

// ========== Debug Info Entry ==========

// XrCoroDebugInfo - Coroutine debug info (creation location and name)
typedef struct XrCoroDebugInfo {
    const char *name;         // Coroutine name (optional)
    const char *source_file;  // Creation location: source file
    int source_line;          // Creation location: line number
    uint64_t create_time_ns;  // Creation time (nanoseconds)
} XrCoroDebugInfo;

// ========== Debug Info Pool ==========

#define XR_DEBUG_POOL_INIT_SIZE 1024          // Initial capacity
#define XR_DEBUG_POOL_MAX_SIZE (1024 * 1024)  // Max capacity
#define XR_DEBUG_IDX_INVALID 0xFFFFFFFF       // Invalid index

// XrCoroDebugPool - Global shared debug info storage
// Uses atomic counter for index allocation, supports concurrent registration
typedef struct XrCoroDebugPool {
    XrCoroDebugInfo *entries;  // Entry array
    _Atomic uint32_t count;    // Current entry count
    uint32_t capacity;         // Capacity
    xr_mutex_t expand_lock;    // Expansion lock
    bool initialized;          // Initialized flag
} XrCoroDebugPool;

// ========== Pool Lifecycle API ==========

// Initialize debug info pool
// Returns true on success
XR_FUNC bool xr_coro_debug_pool_init(XrCoroDebugPool *pool, uint32_t capacity);

// Destroy debug info pool
XR_FUNC void xr_coro_debug_pool_destroy(XrCoroDebugPool *pool);

// ========== Register/Query API ==========

// Register debug info for new coroutine, returns index
// Returns XR_DEBUG_IDX_INVALID on failure
XR_FUNC uint32_t xr_coro_debug_register(XrCoroDebugPool *pool, const char *name, const char *file,
                                        int line);

// Get debug info by index
// Returns NULL for invalid index
XR_FUNC XrCoroDebugInfo *xr_coro_debug_get(XrCoroDebugPool *pool, uint32_t idx);

// ========== Global Pool API (convenience interface) ==========

// Initialize global debug info pool
XR_FUNC bool xr_coro_debug_global_init(void);

// Destroy global debug info pool
XR_FUNC void xr_coro_debug_global_destroy(void);

// Register debug info in global pool
XR_FUNC uint32_t xr_coro_debug_global_register(const char *name, const char *file, int line);

// Get debug info from global pool
XR_FUNC XrCoroDebugInfo *xr_coro_debug_global_get(uint32_t idx);

#endif  // XCORO_DEBUG_H
