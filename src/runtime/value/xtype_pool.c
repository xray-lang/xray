/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_pool.c - Type pool implementation
 *
 * KEY CONCEPT:
 *   Uses arena allocator for types. Memory is never moved once allocated.
 *   All types freed at once when pool is destroyed.
 */

#include "xtype_pool.h"
#include "../../toolchain/xcompiler_arena_backing.h"

static bool pool_ready(XrTypePool *pool) {
    return pool && pool->initialized && xr_compile_state_status(pool->state) == XR_COMPILE_RESOURCE_OK;
}

XrCompileResourceStatus xr_compile_type_pool_open(XrCompileState *state, XrTypePool **output) {
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (!output || *output) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    void *memory = NULL;
    status = xr_compile_state_calloc(state, 1, sizeof(XrTypePool), &memory);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XrTypePool *pool = memory;
    pool->state = state;
    XrArenaBacking backing;
    (void) xr_compiler_arena_state_backing(state, &backing);
    (void) xr_arena_open(&pool->arena, XR_ARENA_SEGMENT_SIZE, &backing);
    status = xr_compiler_arena_capture_status(&pool->arena, state);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_arena_destroy(&pool->arena);
        xr_compile_state_free(pool);
        return status;
    }
    pool->next_type_id = 1;
    pool->initialized = true;
    *output = pool;
    return XR_COMPILE_RESOURCE_OK;
}

void xr_type_pool_free(XrTypePool *pool) {
    if (!pool) return;
    xr_arena_destroy(&pool->arena);
    xr_compile_state_free(pool);
}

void xr_type_pool_reset(XrTypePool *pool) {
    if (!pool_ready(pool)) return;
    xr_arena_reset(&pool->arena);
    if (xr_compiler_arena_capture_status(&pool->arena, pool->state) == XR_COMPILE_RESOURCE_OK)
        pool->next_type_id = 1;
}

void *xr_pool_alloc(XrTypePool *pool, size_t size) {
    if (!pool_ready(pool)) return NULL;
    void *memory = xr_arena_alloc(&pool->arena, size);
    return xr_compiler_arena_capture_status(&pool->arena, pool->state) == XR_COMPILE_RESOURCE_OK ? memory : NULL;
}

void *xr_pool_alloc_array(XrTypePool *pool, size_t elem_size, size_t count) {
    if (!pool_ready(pool)) return NULL;
    void *memory = xr_arena_alloc_array(&pool->arena, elem_size, count);
    return xr_compiler_arena_capture_status(&pool->arena, pool->state) == XR_COMPILE_RESOURCE_OK ? memory : NULL;
}

char *xr_pool_strdup(XrTypePool *pool, const char *str) {
    if (!pool_ready(pool)) return NULL;
    char *copy = xr_arena_strdup(&pool->arena, str);
    return xr_compiler_arena_capture_status(&pool->arena, pool->state) == XR_COMPILE_RESOURCE_OK ? copy : NULL;
}

XrType *xr_pool_alloc_type(XrTypePool *pool, XrTypeKind kind) {
    if (!pool_ready(pool)) return NULL;
    if (pool->next_type_id == UINT32_MAX) {
        xr_compile_state_fail(pool->state, XR_COMPILE_RESOURCE_BUDGET);
        return NULL;
    }
    XrType *type = xr_pool_alloc(pool, sizeof(XrType));
    if (!type) return NULL;
    type->kind = kind;
    type->id = pool->next_type_id++;
    return type;
}
