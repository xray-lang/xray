/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_pool.h - Independent type pool for analyzer
 *
 * KEY CONCEPT:
 *   Type pool provides arena-based allocation for complex types.
 *   Each XaAnalyzer has its own pool. Primitive singletons live in
 *   process-level statics (xtype.c), not in the pool.
 *
 * WHY THIS DESIGN:
 *   - Decoupled from XrayIsolate internals
 *   - Each analyzer instance owns its pool
 *   - Can be unit tested independently
 */

#ifndef XTYPE_POOL_H
#define XTYPE_POOL_H

#include "xtype.h"
#include "../../base/xarena.h"
#include <stdbool.h>
#include <stdint.h>

// Type pool structure
typedef struct XrTypePool {
    bool initialized;
    
    // Per-pool ID counter (no global state)
    uint32_t next_type_id;
    
    // Arena allocator (replaces old realloc-based memory pool)
    XrArena arena;
    
} XrTypePool;

// Pool lifecycle
XR_FUNC XrTypePool *xr_type_pool_new(void);
XR_FUNC void xr_type_pool_free(XrTypePool *pool);
XR_FUNC void xr_type_pool_reset(XrTypePool *pool);

// Allocate a type from pool arena (used by xanalyzer_types.c)
// Types allocated this way are freed when pool is reset or destroyed
XR_FUNC XrType *xr_pool_alloc_type(XrTypePool *pool, XrTypeKind kind);

// Allocate memory from pool arena (for type internal fields)
XR_FUNC void *xr_pool_alloc(XrTypePool *pool, size_t size);
XR_FUNC char *xr_pool_strdup(XrTypePool *pool, const char *str);

#endif // XTYPE_POOL_H
