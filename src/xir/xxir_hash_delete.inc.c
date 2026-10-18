/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_delete.inc.c - Dense removal with preflighted bucket backshift
 */
static XrXirValueStatus hash_delete_preflight(const XirHash *map, size_t entry,
    XrXirValueAdmission *admission) {
    XR_CHECK(entry < map->length, "ordered deletion requires a present dense entry");
    uint64_t moved = (uint64_t)(map->length - entry - 1) * map->stride;
    if (map->capacity > (UINT64_MAX - moved) / 3) return XR_XIR_VALUE_LIMIT;
    /* Admission pays validation, bucket backshift, index rebasing and every
     * moved byte before the first visible change to insertion-ordered storage. */
    XrXirValueStatus status = hash_charge(admission, moved + (uint64_t)map->capacity * 3);
    if (status != XR_XIR_VALUE_OK) return status;
    bool terminated = false;
    for (size_t bucket = 0; bucket < map->capacity; ++bucket) {
        uint64_t slot = hash_buckets(map)[bucket];
        if (!slot) terminated = true;
        if (slot > map->length) return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    return terminated ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
static void hash_delete_commit(XirHash *map, const HashProbe *probe) {
    size_t last = map->length - 1, mask = map->capacity - 1, hole = probe->bucket;
    XirObject *pending = NULL;
    hash_queue_entry(map, probe->entry, &pending);
    size_t bucket = (hole + 1) & mask;
    while (hash_buckets(map)[bucket]) {
        size_t entry = (size_t)hash_buckets(map)[bucket] - 1;
        size_t home = (size_t)hash_entry_hash(map, entry) & mask;
        if (((bucket - home) & mask) >= ((bucket - hole) & mask)) {
            hash_buckets(map)[hole] = hash_buckets(map)[bucket]; hole = bucket;
        }
        bucket = (bucket + 1) & mask;
    }
    hash_buckets(map)[hole] = 0;
    if (probe->entry != last)
        memmove(hash_entry(map, probe->entry), hash_entry(map, probe->entry + 1),
            (last - probe->entry) * map->stride);
    for (size_t i = 0; i < map->capacity; ++i)
        if (hash_buckets(map)[i] > probe->entry + 1) --hash_buckets(map)[i];
    --map->length;
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
        status = hash_delete_preflight(map, probe.entry, admission);
        if (status != XR_XIR_VALUE_OK) return status;
        hash_delete_commit(map, &probe);
    } else {
        XirHash *copy = NULL;
        HashCloneEdit edit = {probe.entry, SIZE_MAX, NULL};
        status = hash_clone(map, map->capacity, &edit, admission, &copy);
        if (status != XR_XIR_VALUE_OK) return status;
        XrXirValue replacement = hash_value(copy);
        memcpy(place->payload, &replacement.payload, sizeof(replacement.payload));
        xr_xir_value_drop(&current);
    }
    *removed = true; return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_hash_delete(const XrXirValuePlace *place, const XrXirValue *key,
    XrXirValueAdmission *admission, bool *removed) {
    if (!hash_admission_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
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
    if (!hash_admission_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = hash_clear(place, admission);
    xr_xir_value_graph_end(); return status;
}
