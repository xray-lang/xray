/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_panic_cache.inc.c - Owned qualified writer proof keys
 */
static void storage_panic_cache_clear(StoragePanicCache *cache) {
    xr_compile_resources_free(cache->keys);
    xr_compile_resources_free(cache->identities);
    xr_compile_resources_free(cache->index);
    *cache = (StoragePanicCache){0};
}

static uint32_t storage_panic_key_hash(StoragePanicKey key, const StoragePanicIdentity *identities) {
    uint32_t fields[] = {key.application, key.target, key.count};
    return xr_hash_bytes(fields, sizeof(fields)) ^
        xr_hash_bytes(identities, (size_t)key.count * sizeof(*identities));
}

static uint32_t storage_panic_cache_slot(const StoragePanicCache *cache,
    StoragePanicKey key, const StoragePanicIdentity *identities) {
    uint32_t slot = storage_panic_key_hash(key, identities) & (cache->index_capacity - 1);
    while (cache->index[slot]) {
        StoragePanicKey old = cache->keys[cache->index[slot] - 1];
        if (old.application == key.application && old.target == key.target && old.count == key.count &&
            (!key.count || !memcmp(cache->identities + old.first, identities,
                (size_t)key.count * sizeof(*identities)))) break;
        slot = (slot + 1) & (cache->index_capacity - 1);
    }
    return slot;
}

static uint32_t storage_panic_cache_find(const StoragePanicCache *cache,
    StoragePanicKey key, const StoragePanicIdentity *identities) {
    if (!cache->index_capacity) return UINT32_MAX;
    uint32_t slot = storage_panic_cache_slot(cache, key, identities);
    return cache->index[slot] ? cache->index[slot] - 1 : UINT32_MAX;
}

static bool storage_panic_cache_index(XirStorageFlow *flow, StoragePanicCache *cache) {
    uint64_t next = cache->index_capacity ? (uint64_t)cache->index_capacity * 2 : 16;
    if (next > UINT32_MAX || next > SIZE_MAX / sizeof(*cache->index)) {
        flow->status = XR_XIR_OVERFLOW;
        return false;
    }
    uint32_t *index = xir_compile_calloc(&flow->memory, (size_t)next, sizeof(*index), &flow->status);
    if (!index) return false;
    for (uint32_t k = 0; k < cache->count; ++k) {
        StoragePanicKey key = cache->keys[k];
        const StoragePanicIdentity *identities = key.count ? cache->identities + key.first : NULL;
        uint32_t slot = storage_panic_key_hash(key, identities) & ((uint32_t)next - 1);
        while (index[slot]) slot = (slot + 1) & ((uint32_t)next - 1);
        index[slot] = k + 1;
    }
    xr_compile_resources_free(cache->index);
    cache->index = index;
    cache->index_capacity = (uint32_t)next;
    return true;
}

static bool storage_panic_cache_add(XirStorageFlow *flow, StoragePanicCache *cache,
    StoragePanicKey key, const StoragePanicIdentity *identities) {
    if (!cache->index_capacity || cache->count >= cache->index_capacity / 2)
        if (!storage_panic_cache_index(flow, cache)) return false;
    if (key.count > UINT32_MAX - cache->identity_count) {
        flow->status = XR_XIR_OVERFLOW;
        return false;
    }
    if (!storage_reserve(flow, (void **)&cache->keys, cache->count, &cache->capacity, sizeof(*cache->keys))) return false;
    uint32_t total = cache->identity_count + key.count;
    while (total > cache->identity_capacity) {
        StoragePanicIdentity *values = xir_compile_array_grow(&flow->memory, cache->identities,
            cache->identity_count, sizeof(*values), &cache->identity_capacity, &flow->status);
        if (!values) return false;
        cache->identities = values;
    }
    uint32_t slot = storage_panic_cache_slot(cache, key, identities);
    if (cache->index[slot]) return true;
    key.first = cache->identity_count;
    if (key.count) memcpy(cache->identities + key.first, identities, (size_t)key.count * sizeof(*identities));
    cache->identity_count = total;
    cache->keys[cache->count] = key;
    cache->index[slot] = ++cache->count;
    return true;
}
