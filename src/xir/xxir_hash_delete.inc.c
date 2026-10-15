/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_delete.inc.c - Dense removal with preflighted bucket backshift
 */
static XrXirValueStatus hash_delete_preflight(const XirHash *map, size_t hole,
    XrXirValueAdmission *admission, size_t *last_bucket) {
    size_t mask = map->capacity - 1, bucket = (hole + 1) & mask;
    bool terminated = false;
    for (size_t count = 0; count < map->capacity; ++count) {
        XrXirValueStatus status = hash_charge(admission, 1);
        if (status != XR_XIR_VALUE_OK) return status;
        uint64_t slot = hash_buckets(map)[bucket];
        if (!slot) { terminated = true; break; }
        if (slot > map->length) return XR_XIR_VALUE_BAD_ARGUMENT;
        bucket = (bucket + 1) & mask;
    }
    if (!terminated) return XR_XIR_VALUE_BAD_ARGUMENT;
    bucket = (size_t)hash_entry_hash(map, map->length - 1) & mask;
    for (size_t count = 0; count < map->capacity; ++count) {
        XrXirValueStatus status = hash_charge(admission, 1);
        if (status != XR_XIR_VALUE_OK) return status;
        uint64_t slot = hash_buckets(map)[bucket];
        if (!slot || slot > map->length) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (slot == map->length) { *last_bucket = bucket; return XR_XIR_VALUE_OK; }
        bucket = (bucket + 1) & mask;
    }
    return XR_XIR_VALUE_BAD_ARGUMENT;
}
static void hash_delete_commit(XirHash *map, const HashProbe *probe, size_t last_bucket) {
    size_t last = map->length - 1, mask = map->capacity - 1, hole = probe->bucket;
    XirObject *pending = NULL;
    hash_queue_entry(map, probe->entry, &pending);
    if (probe->entry != last) {
        memcpy(hash_entry(map, probe->entry), hash_entry(map, last), map->stride);
        hash_buckets(map)[last_bucket] = probe->entry + 1;
    }
    size_t bucket = (hole + 1) & mask;
    while (hash_buckets(map)[bucket]) {
        size_t entry = (size_t)hash_buckets(map)[bucket] - 1;
        size_t home = (size_t)hash_entry_hash(map, entry) & mask;
        if (((bucket - home) & mask) >= ((bucket - hole) & mask)) {
            hash_buckets(map)[hole] = hash_buckets(map)[bucket]; hole = bucket;
        }
        bucket = (bucket + 1) & mask;
    }
    hash_buckets(map)[hole] = 0; --map->length;
    release_pending(pending);
}
static XrXirValueStatus hash_delete(const XrXirValuePlace *place, const XrXirValue *key,
    XrXirValueAdmission *admission, bool *removed) {
    if (!removed) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue current = {0}; XirHash *map = NULL;
    XrXirValueStatus status = hash_place_current(place, 0, admission, &current, &map);
    if (status != XR_XIR_VALUE_OK) return status;
    HashProbe probe = {0};
    status = hash_probe(map, key, admission, &probe);
    if (status != XR_XIR_VALUE_OK) return status;
    if (!probe.found) { *removed = false; return XR_XIR_VALUE_OK; }
    if (atomic_load_explicit(&map->object.references, memory_order_acquire) == 1) {
        size_t last_bucket = 0;
        status = hash_delete_preflight(map, probe.bucket, admission, &last_bucket);
        if (status != XR_XIR_VALUE_OK) return status;
        hash_delete_commit(map, &probe, last_bucket);
    } else {
        XirHash *copy = NULL;
        status = hash_clone(map, map->capacity, probe.entry, admission, &copy);
        if (status != XR_XIR_VALUE_OK) return status;
        XrXirValue replacement = hash_value(copy);
        memcpy(place->payload, &replacement.payload, sizeof(replacement.payload));
        xr_xir_value_drop(&current);
    }
    *removed = true; return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_hash_delete(const XrXirValuePlace *place, const XrXirValue *key,
    XrXirValueAdmission *admission, bool *removed) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = hash_delete(place, key, admission, removed);
    xr_xir_value_graph_end(); return status;
}
static XrXirValueStatus hash_clear(const XrXirValuePlace *place, XrXirValueAdmission *admission) {
    XrXirValue current = {0}; XirHash *map = NULL;
    XrXirValueStatus status = hash_place_current(place, 0, admission, &current, &map);
    if (status != XR_XIR_VALUE_OK || !map->length) return status;
    if (atomic_load_explicit(&map->object.references, memory_order_acquire) == 1) {
        status = hash_charge(admission, (uint64_t)map->length * 2 + map->capacity);
        if (status != XR_XIR_VALUE_OK) return status;
        XirObject *pending = NULL;
        for (size_t i = 0; i < map->length; ++i) hash_queue_entry(map, i, &pending);
        map->length = 0; memset(map->data, 0, map->capacity * sizeof(uint64_t));
        release_pending(pending);
    } else {
        XirHash *copy = NULL;
        status = hash_allocate(map->object.type, 0, admission, &copy);
        if (status != XR_XIR_VALUE_OK) return status;
        XrXirValue replacement = hash_value(copy);
        memcpy(place->payload, &replacement.payload, sizeof(replacement.payload));
        xr_xir_value_drop(&current);
    }
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_hash_clear(const XrXirValuePlace *place, XrXirValueAdmission *admission) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = hash_clear(place, admission);
    xr_xir_value_graph_end(); return status;
}
