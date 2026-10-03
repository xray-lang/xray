/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring_pool.c - Metered compile-time string interning with atomic publication
 */
#include "xstring_pool.h"
#include "../../base/xarena.h"
#include "../../toolchain/xcompiler_arena_backing.h"
#include <string.h>

#define POOL_INIT_CAP 64u

typedef struct PoolEntry {
    uint32_t hash;
    const char *str;
    size_t len;
} PoolEntry;

struct XrCompileStringPool {
    XrArena *arena;
    XrCompileState *state;
    PoolEntry *buckets;
    size_t capacity;
    size_t count;
};

XR_FUNC bool xr_string_pool_matches(const XrCompileStringPool *pool,
                                    XrCompileState *state, const XrArena *arena) {
    return pool && pool->state == state && pool->arena == arena &&
           xr_compiler_arena_matches_state(arena, state);
}

static bool pool_work(XrCompileStringPool *pool, size_t units) {
    return xr_compile_state_work(pool->state, units) == XR_COMPILE_RESOURCE_OK;
}

static void *pool_allocate(XrCompileStringPool *pool, size_t count, size_t size) {
    if (xr_compile_state_status(pool->state) != XR_COMPILE_RESOURCE_OK) return NULL;
    void *memory = xr_arena_alloc_array(pool->arena, size, count);
    return xr_compiler_arena_capture_status(pool->arena, pool->state) == XR_COMPILE_RESOURCE_OK ? memory : NULL;
}

XR_FUNC XrCompileResourceStatus xr_compile_string_pool_open(
    XrCompileState *state, XrArena *arena, XrCompileStringPool **output) {
    if (!output || *output || !xr_compiler_arena_matches_state(arena, state))
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XrCompileResourceStatus status = xr_compiler_arena_capture_status(arena, state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XrCompileStringPool *pool = xr_arena_alloc(arena, sizeof(*pool));
    status = xr_compiler_arena_capture_status(arena, state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    pool->arena = arena;
    pool->state = state;
    pool->buckets = pool_allocate(pool, POOL_INIT_CAP, sizeof(PoolEntry));
    if (!pool->buckets) return xr_compile_state_status(state);
    pool->capacity = POOL_INIT_CAP;
    *output = pool;
    return XR_COMPILE_RESOURCE_OK;
}

static bool pool_rehash(XrCompileStringPool *pool) {
    if (pool->capacity > SIZE_MAX / 2) {
        xr_compile_state_fail(pool->state, XR_COMPILE_RESOURCE_BUDGET);
        return false;
    }
    size_t capacity = pool->capacity * 2;
    PoolEntry *fresh = pool_allocate(pool, capacity, sizeof(*fresh));
    if (!fresh) return false;
    for (size_t i = 0; i < pool->capacity; ++i) {
        if (!pool_work(pool, 1)) return false;
        if (!pool->buckets[i].hash) continue;
        size_t slot = pool->buckets[i].hash & (capacity - 1);
        for (;;) {
            if (!pool_work(pool, 1)) return false;
            if (!fresh[slot].hash) break;
            slot = (slot + 1) & (capacity - 1);
        }
        if (xr_compile_state_copy(pool->state, &fresh[slot], &pool->buckets[i], sizeof(*fresh)) != XR_COMPILE_RESOURCE_OK)
            return false;
    }
    pool->buckets = fresh;
    pool->capacity = capacity;
    return true;
}

static const char *pool_intern(XrCompileStringPool *pool, const char *str, size_t length) {
    if (!pool || xr_compile_state_status(pool->state) != XR_COMPILE_RESOURCE_OK) return NULL;
    if ((!str && length) || length == SIZE_MAX) {
        xr_compile_state_fail(pool->state, length == SIZE_MAX ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return NULL;
    }
    if (pool->count >= pool->capacity - pool->capacity / 4 && !pool_rehash(pool)) return NULL;
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < length; ++i) {
        if (!pool_work(pool, 1)) return NULL;
        hash = (hash ^ (uint8_t) str[i]) * UINT32_C(16777619);
    }
    if (!hash) hash = 1;
    size_t slot = hash & (pool->capacity - 1);
    for (;;) {
        if (!pool_work(pool, 1)) return NULL;
        PoolEntry *entry = &pool->buckets[slot];
        if (!entry->hash) {
            char *copy = xr_arena_strndup(pool->arena, str ? str : "", length);
            if (xr_compiler_arena_capture_status(pool->arena, pool->state) != XR_COMPILE_RESOURCE_OK) return NULL;
            if (!pool_work(pool, sizeof(*entry))) return NULL;
            *entry = (PoolEntry) {hash, copy, length};
            ++pool->count;
            return copy;
        }
        if (entry->hash == hash && entry->len == length) {
            size_t i = 0;
            for (; i < length; ++i) {
                if (!pool_work(pool, 2)) return NULL;
                if (entry->str[i] != str[i]) break;
            }
            if (i == length) return entry->str;
        }
        slot = (slot + 1) & (pool->capacity - 1);
    }
}

XR_FUNC const char *xr_string_pool_intern(XrCompileStringPool *pool, const char *str) {
    if (!pool) return NULL;
    size_t length = 0;
    if (xr_compile_state_string_length(pool->state, str, &length) != XR_COMPILE_RESOURCE_OK) return NULL;
    return pool_intern(pool, str, length);
}

XR_FUNC const char *xr_string_pool_intern_len(XrCompileStringPool *pool, const char *str, size_t length) {
    return pool_intern(pool, str, length);
}

XR_FUNC size_t xr_string_pool_count(const XrCompileStringPool *pool) {
    return pool ? pool->count : 0;
}
