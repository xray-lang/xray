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
#include "../../base/xchecks.h"

static bool pool_ready(XrTypePool *pool) {
    return pool && pool->initialized && xr_compile_state_status(pool->state) == XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xr_compile_type_pool_open(XrCompileState *state, XrTypePool **output) {
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (!output || *output) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    void *memory = NULL;
    status = xr_compile_state_calloc(state, 1, sizeof(XrTypePool), &memory);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XrTypePool *pool = memory;
    pool->state = state;
    atomic_init(&pool->references, 1);
    atomic_init(&pool->creator_released, false);
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

static void xr_type_pool_release_reference(XrTypePool *pool) {
    uint64_t old = atomic_fetch_sub_explicit(&pool->references, 1, memory_order_acq_rel);
    XR_CHECK(old != 0, "Type pool release requires a live reference");
    if (old != 1)
        return;
    XR_CHECK(atomic_load_explicit(&pool->creator_released, memory_order_acquire),
             "The final pool reference requires a closed creator");
    xr_arena_destroy(&pool->arena);
    xr_compile_state_free(pool);
}

XR_FUNC void xr_type_pool_free(XrTypePool *pool) {
    if (!pool)
        return;
    bool released = atomic_exchange_explicit(&pool->creator_released, true, memory_order_acq_rel);
    XR_CHECK(!released, "The type pool creator may be released exactly once");
    xr_type_pool_release_reference(pool);
}

XR_FUNC XrCompileResourceStatus xr_type_pool_pin(XrCompileState *state, XrTypePool *pool,
                                                XrTypePoolPin *output) {
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    if (!output || output->pool || !pool || !pool->initialized || pool->state != state)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    uint64_t current = atomic_load_explicit(&pool->references, memory_order_relaxed);
    for (;;) {
        XR_CHECK(current != 0, "Pinning requires a live pool owner");
        if (current == UINT64_MAX)
            return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_OVERFLOW);
        if (atomic_compare_exchange_weak_explicit(&pool->references, &current, current + 1,
                memory_order_relaxed, memory_order_relaxed))
            break;
    }
    output->pool = pool;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC void xr_type_pool_unpin(XrTypePoolPin *pin) {
    if (!pin || !pin->pool)
        return;
    XrTypePool *pool = pin->pool;
    pin->pool = NULL;
    xr_type_pool_release_reference(pool);
}

XR_FUNC XrCompileResourceStatus xr_type_pool_reset(XrTypePool *pool) {
    XrCompileState *state = pool ? pool->state : NULL;
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    if (!pool->initialized || atomic_load_explicit(&pool->creator_released, memory_order_acquire) ||
        atomic_load_explicit(&pool->references, memory_order_acquire) != 1)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    xr_arena_reset(&pool->arena);
    status = xr_compiler_arena_capture_status(&pool->arena, state);
    if (status == XR_COMPILE_RESOURCE_OK)
        pool->next_type_id = 1;
    return status;
}

XR_FUNC void *xr_pool_alloc(XrTypePool *pool, size_t size) {
    if (!pool_ready(pool)) return NULL;
    void *memory = xr_arena_alloc(&pool->arena, size);
    return xr_compiler_arena_capture_status(&pool->arena, pool->state) == XR_COMPILE_RESOURCE_OK ? memory : NULL;
}

XR_FUNC void *xr_pool_alloc_array(XrTypePool *pool, size_t elem_size, size_t count) {
    if (!pool_ready(pool)) return NULL;
    void *memory = xr_arena_alloc_array(&pool->arena, elem_size, count);
    return xr_compiler_arena_capture_status(&pool->arena, pool->state) == XR_COMPILE_RESOURCE_OK ? memory : NULL;
}

XR_FUNC char *xr_pool_strdup(XrTypePool *pool, const char *str) {
    if (!pool_ready(pool)) return NULL;
    char *copy = xr_arena_strdup(&pool->arena, str);
    return xr_compiler_arena_capture_status(&pool->arena, pool->state) == XR_COMPILE_RESOURCE_OK ? copy : NULL;
}

XR_FUNC XrType *xr_pool_alloc_type(XrTypePool *pool, XrTypeKind kind) {
    if (!pool_ready(pool)) return NULL;
    if (pool->next_type_id == UINT32_MAX) {
        xr_compile_state_fail(pool->state, XR_COMPILE_RESOURCE_OVERFLOW);
        return NULL;
    }
    XrType *type = xr_pool_alloc(pool, sizeof(XrType));
    if (!type) return NULL;
    type->kind = kind;
    type->id = pool->next_type_id++;
    return type;
}
