/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring_pool.c - Global string intern pool lifecycle and maintenance.
 */

#include "xstring.h"
#include "../../base/xchecks.h"
#include "../../base/xhash.h"
#include "../../base/xlog.h"
#include "../../base/xmalloc.h"
#include <string.h>

void xr_global_pool_init(XrGlobalStringPool *pool) {
    if (!pool)
        return;

    pool->capacity = GLOBAL_POOL_INIT_CAPACITY;
    pool->mask = pool->capacity - 1;
    pool->count = 0;
    pool->permanent_count = 0;
    pool->entries = (XrString **) xr_malloc(sizeof(XrString *) * pool->capacity);
    if (!pool->entries)
        return;

    xr_rwlock_init(&pool->lock);
    memset(pool->entries, 0, sizeof(XrString *) * pool->capacity);
}

void xr_global_pool_free(XrGlobalStringPool *pool) {
    if (!pool || !pool->entries)
        return;

    xr_rwlock_destroy(&pool->lock);
    for (size_t i = 0; i < pool->capacity; i++) {
        if (pool->entries[i] != NULL)
            xr_free(pool->entries[i]);
    }

    xr_free(pool->entries);
    pool->entries = NULL;
    pool->capacity = 0;
    pool->count = 0;
}

static void global_pool_grow(XrGlobalStringPool *pool) {
    if (!pool)
        return;
    XR_DCHECK((pool->capacity & (pool->capacity - 1)) == 0,
              "global_pool_grow: capacity not power-of-2");

    size_t old_capacity = pool->capacity;
    XrString **old_entries = pool->entries;

    pool->capacity = old_capacity * 2;
    pool->mask = pool->capacity - 1;
    pool->entries = (XrString **) xr_malloc(sizeof(XrString *) * pool->capacity);
    if (!pool->entries) {
        pool->entries = old_entries;
        pool->capacity = old_capacity;
        pool->mask = old_capacity - 1;
        return;
    }

    memset(pool->entries, 0, sizeof(XrString *) * pool->capacity);

    size_t saved_count = pool->count;
    pool->count = 0;

    for (size_t i = 0; i < old_capacity; i++) {
        XrString *str = old_entries[i];
        if (str) {
            uint32_t index = str->hash & pool->mask;
            while (pool->entries[index] != NULL)
                index = (index + 1) & pool->mask;
            pool->entries[index] = str;
            pool->count++;
        }
    }

    (void) saved_count;
    XR_DCHECK(pool->count == saved_count, "global_pool_grow: count mismatch after rehash");
    xr_free(old_entries);
}

XrString *xr_global_pool_insert_locked(XrGlobalStringPool *pool, const char *chars, size_t len,
                                       uint32_t hash) {
    if (!pool)
        return NULL;

    if (pool->count >= GLOBAL_POOL_WARN_THRESHOLD) {
        static _Atomic int warned = 0;
        if (!warned) {
            warned = 1;
            xr_log_warning(
                "string",
                "XrGlobalStringPool: %zu strings (permanent: %zu), consider investigating",
                pool->count, pool->permanent_count);
        }
    }

    if (hash == 0)
        hash = xr_hash_bytes(chars, len);

    size_t mask = pool->mask;
    uint32_t index = hash & mask;

    for (;;) {
        XrString *entry = pool->entries[index];

        if (entry == NULL) {
            size_t total_size = sizeof(XrString) + len + 1;
            XrString *str = (XrString *) xr_malloc(total_size);
            if (!str)
                return NULL;

            memset(&str->hdr, 0, sizeof(XrObjHeader));
            str->hdr.type = XR_TSTRING;
            str->hdr.objsize = (uint32_t) total_size;

            str->length = (uint32_t) len;
            str->hash = hash;
            memcpy(str->data, chars, len);
            str->data[len] = '\0';

            XR_STR_SET_GLOBAL(str);
            XR_OBJ_SET_STORAGE(&str->hdr, XR_OBJ_STORAGE_SHARED);

            pool->entries[index] = str;
            pool->count++;

            if (pool->count > pool->capacity * 3 / 4)
                global_pool_grow(pool);

            return str;
        }

        if (entry->length == len && entry->hash == hash && memcmp(entry->data, chars, len) == 0)
            return entry;

        index = (index + 1) & mask;
    }
}

XrString *xr_global_pool_insert(XrGlobalStringPool *pool, XrayIsolate *iso, const char *chars,
                                size_t len, uint32_t hash) {
    XR_DCHECK(pool != NULL, "global_pool_insert: NULL pool");
    XR_DCHECK(chars != NULL, "global_pool_insert: NULL chars");
    (void) iso;
    XrString *str = xr_global_pool_insert_locked(pool, chars, len, hash);
    if (str && !XR_STR_IS_PERMANENT(str)) {
        XR_STR_SET_PERMANENT(str);
        pool->permanent_count++;
    }
    return str;
}

void xr_global_pool_freeze(XrGlobalStringPool *pool) {
    (void) pool;
}

XrString *xr_global_pool_lookup(XrGlobalStringPool *pool, const char *chars, size_t len,
                                uint32_t hash) {
    if (!pool || !pool->entries)
        return NULL;

    size_t mask = pool->mask;
    uint32_t index = hash & mask;

    for (;;) {
        XrString *entry = pool->entries[index];

        if (entry == NULL)
            return NULL;

        if (entry->length == len && entry->hash == hash && memcmp(entry->data, chars, len) == 0) {
            if (!XR_STR_IS_PERMANENT(entry))
                XR_STR_SET_ACCESSED(entry);
            return entry;
        }

        index = (index + 1) & mask;
    }
}

size_t xr_global_pool_sweep(XrGlobalStringPool *pool) {
    if (!pool || !pool->entries)
        return 0;

    xr_rwlock_wrlock(&pool->lock);

    size_t evicted = 0;
    size_t cap = pool->capacity;

    for (size_t i = 0; i < cap; i++) {
        XrString *str = pool->entries[i];
        if (!str)
            continue;

        if (XR_STR_IS_PERMANENT(str)) {
            XR_STR_CLR_ACCESSED(str);
            continue;
        }

        if (XR_STR_IS_ACCESSED(str)) {
            XR_STR_CLR_ACCESSED(str);
            continue;
        }

        xr_free(str);
        pool->entries[i] = NULL;
        pool->count--;
        evicted++;
    }

    if (evicted > 0) {
        size_t old_cap = cap;
        XrString **old = pool->entries;

        size_t new_cap = cap;
        if (pool->count < cap / 4 && cap > GLOBAL_POOL_INIT_CAPACITY) {
            new_cap = GLOBAL_POOL_INIT_CAPACITY;
            while (new_cap < pool->count * 2)
                new_cap *= 2;
            if (new_cap > cap)
                new_cap = cap;
        }

        pool->capacity = new_cap;
        pool->mask = new_cap - 1;
        pool->entries = (XrString **) xr_malloc(sizeof(XrString *) * new_cap);
        if (!pool->entries) {
            pool->entries = old;
            pool->capacity = old_cap;
            pool->mask = old_cap - 1;
            xr_rwlock_wrunlock(&pool->lock);
            return evicted;
        }
        memset(pool->entries, 0, sizeof(XrString *) * new_cap);

        size_t new_count = 0;
        size_t mask = pool->mask;
        for (size_t i = 0; i < old_cap; i++) {
            XrString *str = old[i];
            if (!str)
                continue;
            uint32_t idx = str->hash & mask;
            while (pool->entries[idx] != NULL)
                idx = (idx + 1) & mask;
            pool->entries[idx] = str;
            new_count++;
        }
        pool->count = new_count;
        xr_free(old);
    }

    xr_rwlock_wrunlock(&pool->lock);
    return evicted;
}
