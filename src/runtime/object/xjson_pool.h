/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson_pool.h - Json object pool for high-frequency allocations
 *
 * KEY CONCEPT:
 *   Pool management for short-lived Json objects to reduce malloc/free
 *   and GC pressure. Objects are grouped by field count (0-7 fields).
 *
 * WHY THIS DESIGN:
 *   - Pools grouped by field count for efficient reuse
 *   - Each pool caches up to POOL_MAX_SIZE objects
 *   - Falls back to normal GC release when pool is full
 */

#ifndef XJSON_POOL_H
#define XJSON_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include "../../base/xdefs.h"

// Forward declarations
struct XrJson;
struct XrShape;
struct XrayIsolate;

/* ========== Pool Configuration ========== */

#define XR_JSON_POOL_MAX_SIZE 2048  // Max objects per pool
#define XR_JSON_POOL_MAX_FIELDS 8   // Objects with more fields are not pooled

/* ========== Pool Structures ========== */

// Single Json pool for objects with specific field count
typedef struct XrJsonPool {
    struct XrJson **slots;
    int count;
    int capacity;
    int field_count;

    // Stats
    uint64_t hits;
    uint64_t misses;
    uint64_t returns;
    uint64_t overflows;  // Released when pool is full
} XrJsonPool;

// Json pool manager
typedef struct XrJsonPoolManager {
    XrJsonPool pools[XR_JSON_POOL_MAX_FIELDS];  // Pools grouped by field count
    bool enabled;

    // Global stats
    uint64_t total_allocs;
    uint64_t pool_allocs;
    uint64_t heap_allocs;
} XrJsonPoolManager;

/* ========== API ========== */

XR_FUNC void xr_json_pool_init(XrJsonPoolManager *pm);
XR_FUNC void xr_json_pool_cleanup(XrJsonPoolManager *pm);

// Get Json from pool. Returns NULL if pool miss (caller should allocate).
XR_FUNC struct XrJson *xr_json_pool_get(XrJsonPoolManager *pm, struct XrShape *shape,
                                        int field_count);

// Return Json to pool. Returns true if returned, false if should be freed.
XR_FUNC bool xr_json_pool_return(XrJsonPoolManager *pm, struct XrJson *json);

XR_FUNC void xr_json_pool_dump_stats(XrJsonPoolManager *pm);
XR_FUNC void xr_json_pool_set_enabled(XrJsonPoolManager *pm, bool enabled);

#endif  // XJSON_POOL_H
