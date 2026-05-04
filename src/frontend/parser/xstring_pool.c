/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring_pool.c - Compile-time string deduplication pool
 *
 * KEY CONCEPT:
 *   Open-addressing hash table (Robin Hood probing) backed entirely
 *   by the parser arena. Deduplicates string literals and identifiers
 *   so that identical text shares a single pointer.
 *
 *   Rehash doubles the bucket array (arena-allocated; old array is
 *   abandoned in the arena — acceptable since the arena is freed
 *   wholesale after parsing).
 */

#include "xstring_pool.h"
#include "../../base/xarena.h"
#include "../../base/xchecks.h"
#include "../../base/xhash.h"
#include <string.h>

/* Load factor threshold: rehash when count > capacity * 3/4. */
#define POOL_LOAD_NUM   3
#define POOL_LOAD_DEN   4
#define POOL_INIT_CAP   64

/* Each bucket stores a cached hash, string pointer, and length. */
typedef struct {
    uint32_t    hash;   /* 0 = empty */
    const char *str;
    size_t      len;
} PoolEntry;

struct XrCompileStringPool {
    XrArena    *arena;
    PoolEntry  *buckets;
    size_t      capacity;   /* always a power of two */
    size_t      count;
};

/* ------------------------------------------------------------------ */

XR_FUNC XrCompileStringPool *xr_string_pool_new(XrArena *arena) {
    XR_DCHECK(arena != NULL, "xr_string_pool_new: NULL arena");

    XrCompileStringPool *pool =
        (XrCompileStringPool *)xr_arena_alloc(arena, sizeof(XrCompileStringPool));
    pool->arena    = arena;
    pool->capacity = POOL_INIT_CAP;
    pool->count    = 0;
    pool->buckets  = (PoolEntry *)xr_arena_alloc(arena,
                         sizeof(PoolEntry) * POOL_INIT_CAP);
    memset(pool->buckets, 0, sizeof(PoolEntry) * POOL_INIT_CAP);
    return pool;
}

/* Rehash into a new, larger bucket array. Old array stays in the arena. */
static void pool_rehash(XrCompileStringPool *pool) {
    size_t old_cap = pool->capacity;
    PoolEntry *old = pool->buckets;

    size_t new_cap = old_cap * 2;
    PoolEntry *fresh = (PoolEntry *)xr_arena_alloc(pool->arena,
                           sizeof(PoolEntry) * new_cap);
    memset(fresh, 0, sizeof(PoolEntry) * new_cap);

    size_t mask = new_cap - 1;
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].hash == 0)
            continue;
        uint32_t h = old[i].hash;
        size_t idx = h & mask;
        while (fresh[idx].hash != 0)
            idx = (idx + 1) & mask;
        fresh[idx] = old[i];
    }

    pool->buckets  = fresh;
    pool->capacity = new_cap;
}

/* Core lookup-or-insert.  Returns the canonical pointer. */
static const char *pool_intern(XrCompileStringPool *pool,
                               const char *str, size_t len) {
    XR_DCHECK(pool != NULL, "pool_intern: NULL pool");

    /* Rehash if above load threshold. */
    if (pool->count * POOL_LOAD_DEN >= pool->capacity * POOL_LOAD_NUM)
        pool_rehash(pool);

    uint32_t h = xr_hash_bytes(str, len);
    if (h == 0) h = 1;  /* reserve 0 as empty sentinel */

    size_t mask = pool->capacity - 1;
    size_t idx  = h & mask;

    for (;;) {
        PoolEntry *e = &pool->buckets[idx];
        if (e->hash == 0) {
            /* Empty slot — insert. */
            char *dup = xr_arena_strndup(pool->arena, str, len);
            e->hash = h;
            e->str  = dup;
            e->len  = len;
            pool->count++;
            return dup;
        }
        if (e->hash == h && e->len == len && memcmp(e->str, str, len) == 0) {
            /* Hit — return existing. */
            return e->str;
        }
        idx = (idx + 1) & mask;
    }
}

XR_FUNC const char *xr_string_pool_intern(XrCompileStringPool *pool,
                                           const char *str) {
    if (!str) return NULL;
    return pool_intern(pool, str, strlen(str));
}

XR_FUNC const char *xr_string_pool_intern_len(XrCompileStringPool *pool,
                                               const char *str, size_t len) {
    if (!str) return NULL;
    return pool_intern(pool, str, len);
}

XR_FUNC size_t xr_string_pool_count(const XrCompileStringPool *pool) {
    XR_DCHECK(pool != NULL, "xr_string_pool_count: NULL pool");
    return pool->count;
}
