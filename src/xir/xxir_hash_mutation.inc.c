/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_mutation.inc.c - Owned root transactions for Map and Set
 */
static XrXirValueStatus hash_admit_field(const XrXirValue *value, XrXirType type,
    XrXirValueAdmission *admission, bool *sensitive) {
    *sensitive = false;
    if (type != XR_XIR_UNIT) return value_admit_summary(value, type, admission, sensitive);
    return unit_value(value) ? hash_charge(admission, 1) : XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirValue hash_stored_key(const XirHash *map, size_t index) {
    XrXirLayout layout = {0};
    XR_CHECK(xr_xir_compile_type_arena_layout(map->object.arena, map->key_type, &layout),
        "sealed hash keys have compact leaf storage");
    return storage_leaf_value(map->key_type, hash_span(map, index, false).bytes, layout.size);
}
static void hash_publish_prepared(XirHash *map, uint64_t hash, size_t bucket,
    StoragePrepared *key, StoragePrepared *value) {
    size_t index = map->length;
    XR_CHECK(index < map->capacity / 2, "hash insertion keeps a vacant probe terminator");
    memcpy(hash_entry(map, index), &hash, sizeof(hash));
    hash_buckets(map)[bucket] = index + 1;
    hash_transfer(key, map, index, false);
    if (value->owns) hash_transfer(value, map, index, true);
    ++map->length;
}
static XrXirValueStatus hash_append_prepared(XirHash *map, uint64_t hash,
    StoragePrepared *key, StoragePrepared *value, XrXirValueAdmission *admission) {
    size_t bucket = 0;
    XrXirValueStatus status = hash_empty_bucket(map, hash, admission, &bucket);
    if (status == XR_XIR_VALUE_OK) hash_publish_prepared(map, hash, bucket, key, value);
    return status;
}
static XrXirValueStatus hash_upsert(const XrXirValuePlace *place, const XrXirValue *key,
    const XrXirValue *value, XrXirValueAdmission *admission) {
    XrXirValue current = {0}; XirHash *map = NULL;
    XrXirValueStatus status = hash_place_current(place,
        value ? XR_XIR_TYPE_MAP : XR_XIR_TYPE_SET, admission, &current, &map);
    if (status != XR_XIR_VALUE_OK) return status;
    HashProbe probe = {0};
    status = hash_probe(map, key, admission, &probe);
    if (status != XR_XIR_VALUE_OK || (!value && probe.found)) return status;
    bool sensitive = false;
    if (value) {
        status = hash_admit_field(value, map->value_type, admission, &sensitive);
        if (status != XR_XIR_VALUE_OK) return status;
    }
    size_t length = map->length + (probe.found ? 0u : 1u), capacity = 0;
    if (!hash_capacity(length, map->stride, &capacity)) return XR_XIR_VALUE_LIMIT;
    if (capacity < map->capacity) capacity = map->capacity;
    bool unique = atomic_load_explicit(&map->object.references, memory_order_acquire) == 1;
    bool grow = length > map->capacity / 2;
    bool detach = map->object.domain != admission->domain && sensitive;
    bool inplace = unique && !detach && (!grow || map->object.domain == admission->domain);
    StoragePrepared prepared_key = {0}, prepared_value = {0};
    XrXirValue stored = probe.found ? hash_stored_key(map, probe.entry) : *key;
    if (!probe.found || !inplace) {
        status = hash_prepare_field(&prepared_key, &stored, map->key_type, admission);
        if (status != XR_XIR_VALUE_OK) return status;
    }
    if (value) status = hash_prepare_field(&prepared_value, value, map->value_type, admission);
    if (status != XR_XIR_VALUE_OK) { storage_prepared_end(&prepared_key); return status; }
    XirObject *pending = NULL;
    if (inplace && !probe.found) {
        /* A new table requires a fresh empty bucket. All fallible probing and
         * work are complete before its first dense owner is transferred. */
        size_t bucket = probe.bucket;
        if (grow) status = hash_grow(map, capacity, probe.hash, admission, &bucket);
        if (status == XR_XIR_VALUE_OK)
            hash_publish_prepared(map, probe.hash, bucket, &prepared_key, &prepared_value);
    } else if (inplace) {
        hash_queue_field(map, probe.entry, true, &pending);
        hash_transfer(&prepared_value, map, probe.entry, true);
    } else {
        XirHash *replacement = NULL;
        status = hash_clone(map, capacity, probe.found ? probe.entry : SIZE_MAX, admission, &replacement);
        if (status == XR_XIR_VALUE_OK) {
            XrXirValue result = hash_value(replacement);
            status = hash_append_prepared(replacement, probe.hash, &prepared_key, &prepared_value, admission);
            if (status == XR_XIR_VALUE_OK) {
                memcpy(place->payload, &result.payload, sizeof(result.payload));
                xr_xir_value_drop(&current);
            } else xr_xir_value_drop(&result);
        }
    }
    release_pending(pending);
    storage_prepared_end(&prepared_value); storage_prepared_end(&prepared_key);
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_map_set(const XrXirValuePlace *place, const XrXirValue *key,
    const XrXirValue *value, XrXirValueAdmission *admission) {
    if (!value || !admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = hash_upsert(place, key, value, admission);
    xr_xir_value_graph_end(); return status;
}
XR_FUNC XrXirValueStatus xr_xir_set_add(const XrXirValuePlace *place, const XrXirValue *element,
    XrXirValueAdmission *admission) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = hash_upsert(place, element, NULL, admission);
    xr_xir_value_graph_end(); return status;
}
