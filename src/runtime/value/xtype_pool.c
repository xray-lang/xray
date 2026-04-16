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
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include <string.h>

// Allocate from pool arena
static void *pool_alloc(XrTypePool *pool, size_t size) {
    XR_DCHECK(pool != NULL, "pool_alloc: NULL pool");
    XR_DCHECK(size > 0, "pool_alloc: zero size");
    return xr_arena_alloc(&pool->arena, size);
}

static char *pool_strdup(XrTypePool *pool, const char *str) {
    XR_DCHECK(pool != NULL, "pool_strdup: NULL pool");
    XR_DCHECK(str != NULL, "pool_strdup: NULL string");
    return xr_arena_strdup(&pool->arena, str);
}

// Create a type within pool
static XrType *pool_type_new(XrTypePool *pool, XrTypeKind kind) {
    XR_DCHECK(pool != NULL, "pool_type_new: NULL pool");
    XrType *type = pool_alloc(pool, sizeof(XrType));
    if (!type) return NULL;
    memset(type, 0, sizeof(XrType));
    type->kind = kind;
    return type;
}

XrTypePool *xr_type_pool_new(void) {
    XrTypePool *pool = xr_calloc(1, sizeof(XrTypePool));
    if (!pool) return NULL;
    
    xr_arena_init(&pool->arena, XR_ARENA_SEGMENT_SIZE);
    pool->next_type_id = 1;
    pool->initialized = true;
    
    return pool;
}

void xr_type_pool_free(XrTypePool *pool) {
    if (!pool) return;
    xr_arena_destroy(&pool->arena);
    xr_free(pool);
}

void xr_type_pool_reset(XrTypePool *pool) {
    if (!pool) return;
    xr_arena_reset(&pool->arena);
    pool->next_type_id = 1;
}

// Allocate a type from pool arena (public API for xanalyzer_types.c)
XrType *xr_pool_alloc_type(XrTypePool *pool, XrTypeKind kind) {
    if (!pool) return NULL;
    XrType *type = pool_type_new(pool, kind);
    if (type) {
        type->id = pool->next_type_id++;
    }
    return type;
}

// Allocate memory from pool arena (public API for type internal fields)
void *xr_pool_alloc(XrTypePool *pool, size_t size) {
    if (!pool) return NULL;
    return pool_alloc(pool, size);
}

// Duplicate string in pool arena
char *xr_pool_strdup(XrTypePool *pool, const char *str) {
    if (!pool) return NULL;
    return pool_strdup(pool, str);
}

