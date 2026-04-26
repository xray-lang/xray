/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_debug.c - Coroutine debug info pool implementation
 *
 * KEY CONCEPT:
 *   Million-concurrency optimization: externalize debug info from coroutine struct.
 */

#include "xcoro_debug.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ========== Thread-Local Debug Info Pool (per-Isolate support) ==========

static __thread XrCoroDebugPool tls_debug_pool = {0};

// ========== Helper Functions ==========

// Get current time (nanoseconds)
static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Expand pool capacity
static bool expand_pool(XrCoroDebugPool *pool) {
    uint32_t new_capacity = pool->capacity * 2;
    if (new_capacity > XR_DEBUG_POOL_MAX_SIZE) {
        new_capacity = XR_DEBUG_POOL_MAX_SIZE;
    }
    if (new_capacity <= pool->capacity) {
        return false;  // Already at max capacity
    }

    if (!XR_REALLOC(pool->entries, new_capacity * sizeof(XrCoroDebugInfo))) {
        return false;
    }

    // Zero new portion
    memset(pool->entries + pool->capacity, 0,
           (new_capacity - pool->capacity) * sizeof(XrCoroDebugInfo));

    pool->capacity = new_capacity;

    return true;
}

// ========== Pool Lifecycle API ==========

bool xr_coro_debug_pool_init(XrCoroDebugPool *pool, uint32_t capacity) {
    if (!pool)
        return false;

    memset(pool, 0, sizeof(XrCoroDebugPool));

    if (capacity == 0) {
        capacity = XR_DEBUG_POOL_INIT_SIZE;
    }

    pool->entries = xr_calloc(capacity, sizeof(XrCoroDebugInfo));
    if (!pool->entries) {
        return false;
    }

    pool->capacity = capacity;
    atomic_store(&pool->count, 0);

    if (pthread_mutex_init(&pool->expand_lock, NULL) != 0) {
        xr_free(pool->entries);
        pool->entries = NULL;
        return false;
    }

    pool->initialized = true;
    return true;
}

void xr_coro_debug_pool_destroy(XrCoroDebugPool *pool) {
    if (!pool || !pool->initialized)
        return;

    if (pool->entries) {
        xr_free(pool->entries);
        pool->entries = NULL;
    }

    pthread_mutex_destroy(&pool->expand_lock);
    pool->initialized = false;
}

// ========== Register/Query API ==========

uint32_t xr_coro_debug_register(XrCoroDebugPool *pool, const char *name, const char *file,
                                int line) {
    if (!pool || !pool->initialized) {
        return XR_DEBUG_IDX_INVALID;
    }

    // Atomic index allocation
    uint32_t idx = atomic_fetch_add(&pool->count, 1);

    // Check if expansion needed
    if (idx >= pool->capacity) {
        pthread_mutex_lock(&pool->expand_lock);

        // Double check
        if (idx >= pool->capacity) {
            if (!expand_pool(pool)) {
                pthread_mutex_unlock(&pool->expand_lock);
                // Allocation failed, rollback count
                atomic_fetch_sub(&pool->count, 1);
                return XR_DEBUG_IDX_INVALID;
            }
        }

        pthread_mutex_unlock(&pool->expand_lock);
    }

    // Fill debug info
    XrCoroDebugInfo *info = &pool->entries[idx];
    info->name = name;
    info->source_file = file;
    info->source_line = line;
    info->create_time_ns = get_time_ns();

    return idx;
}

XrCoroDebugInfo *xr_coro_debug_get(XrCoroDebugPool *pool, uint32_t idx) {
    if (!pool || !pool->initialized) {
        return NULL;
    }

    if (idx == XR_DEBUG_IDX_INVALID || idx >= atomic_load(&pool->count)) {
        return NULL;
    }

    return &pool->entries[idx];
}

// ========== Thread-Local Pool API (per-Isolate support) ==========

bool xr_coro_debug_global_init(void) {
    return xr_coro_debug_pool_init(&tls_debug_pool, 0);
}

void xr_coro_debug_global_destroy(void) {
    xr_coro_debug_pool_destroy(&tls_debug_pool);
}

uint32_t xr_coro_debug_global_register(const char *name, const char *file, int line) {
    return xr_coro_debug_register(&tls_debug_pool, name, file, line);
}

XrCoroDebugInfo *xr_coro_debug_global_get(uint32_t idx) {
    return xr_coro_debug_get(&tls_debug_pool, idx);
}
