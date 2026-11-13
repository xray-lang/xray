/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_delete.inc.c - Dense removal with preflighted bucket backshift
 */
typedef struct HashDeletePlan { size_t end, last_bucket; } HashDeletePlan;
static XrXirValueStatus hash_delete_preflight(const XirHash *map, const HashProbe *probe,
    XrXirValueAdmission *admission, HashDeletePlan *plan) {
    size_t last = map->length - 1, mask = map->capacity - 1;
    XR_CHECK(probe->entry <= last, "deletion requires a present dense owner");
    XrXirValueStatus status = hash_charge(admission,
        (probe->entry == last ? 0u : map->stride) + 2 * hash_order_work(map->capacity));
    if (status != XR_XIR_VALUE_OK) return status;
    size_t position = hash_entry_order(map, probe->entry);
    if (position >= map->order_count || hash_order(map)[position] != probe->entry + 1)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    for (size_t p = position; p < map->capacity; p |= p + 1)
        if (!hash_order_tree(map)[p] || hash_order_tree(map)[p] > map->length)
            return XR_XIR_VALUE_BAD_ARGUMENT;
    if (probe->entry != last) {
        position = hash_entry_order(map, last);
        if (position >= map->order_count || hash_order(map)[position] != last + 1)
            return XR_XIR_VALUE_BAD_ARGUMENT;
        size_t bucket = (size_t)hash_entry_hash(map, last) & mask;
        bool found = false;
        for (size_t n = 0; n < map->capacity; ++n, bucket = (bucket + 1) & mask) {
            status = hash_charge(admission, 1);
            if (status != XR_XIR_VALUE_OK) return status;
            uint64_t slot = hash_buckets(map)[bucket];
            if (!slot || slot > map->length) return XR_XIR_VALUE_BAD_ARGUMENT;
            if (slot == last + 1) { plan->last_bucket = bucket; found = true; break; }
        }
        if (!found) return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    size_t bucket = (probe->bucket + 1) & mask;
    for (size_t n = 0; n < map->capacity; ++n, bucket = (bucket + 1) & mask) {
        status = hash_charge(admission, 1);
        if (status != XR_XIR_VALUE_OK) return status;
        uint64_t slot = hash_buckets(map)[bucket];
        if (!slot) { plan->end = bucket; return XR_XIR_VALUE_OK; }
        if (slot > map->length) return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    return XR_XIR_VALUE_BAD_ARGUMENT;
}
static void hash_delete_commit(XirHash *map, const HashProbe *probe, HashDeletePlan plan) {
    size_t last = map->length - 1, mask = map->capacity - 1, hole = probe->bucket;
    XirObject *pending = NULL;
    hash_queue_entry(map, probe->entry, &pending);
    hash_order_remove(map, probe->entry);
    for (size_t bucket = (hole + 1) & mask; bucket != plan.end; bucket = (bucket + 1) & mask) {
        size_t entry = (size_t)hash_buckets(map)[bucket] - 1;
        size_t home = (size_t)hash_entry_hash(map, entry) & mask;
        if (((bucket - home) & mask) >= ((bucket - hole) & mask)) {
            hash_buckets(map)[hole] = hash_buckets(map)[bucket];
            if (entry == last) plan.last_bucket = hole;
            hole = bucket;
        }
    }
    hash_buckets(map)[hole] = 0;
    if (probe->entry != last) {
        memcpy(hash_entry(map, probe->entry), hash_entry(map, last), map->stride);
        hash_buckets(map)[plan.last_bucket] = probe->entry + 1;
        hash_order(map)[hash_entry_order(map, probe->entry)] = probe->entry + 1;
    }
    --map->length;
    if (!map->length) map->order_count = 0;
    release_pending(pending);
}
static XrXirValueStatus hash_delete(const XrXirValuePlace *place, const XrXirValue *key,
    XrXirValueAdmission *admission, bool *removed, XrXirValue *retired) {
    if (!removed) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue current = {0}; XirHash *map = NULL;
    XrXirValueStatus status = hash_place_current(place, 0, admission, &current, &map);
    if (status != XR_XIR_VALUE_OK) return status;
    HashProbe probe = {0};
    status = hash_probe(map, key, admission, &probe);
    if (status != XR_XIR_VALUE_OK) return status;
    if (!probe.found) { *removed = false; return XR_XIR_VALUE_OK; }
    if (!value_place_shared(place) && xir_object_unique(&map->object)) {
        HashDeletePlan plan = {0};
        status = hash_delete_preflight(map, &probe, admission, &plan);
        if (status != XR_XIR_VALUE_OK) return status;
        hash_delete_commit(map, &probe, plan);
    } else {
        XirHash *copy = NULL;
        HashCloneEdit edit = {probe.entry, SIZE_MAX, NULL};
        status = hash_clone(map, map->capacity, &edit, admission, &copy);
        if (status != XR_XIR_VALUE_OK) return status;
        XrXirValue replacement = hash_value(copy);
        status = value_place_publish(place, &replacement);
        if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&replacement); return status; }
        hash_publish_replacement(place, &replacement, &current, retired);
    }
    *removed = true; return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_hash_delete(const XrXirValuePlace *place, const XrXirValue *key,
    XrXirValueAdmission *admission, bool *removed) {
    if (!hash_admission_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = hash_delete(place, key, admission, removed, NULL);
    xr_xir_value_graph_end(); return status;
}
static XrXirValueStatus hash_clear(const XrXirValuePlace *place, XrXirValueAdmission *admission,
    XrXirValue *retired) {
    XrXirValue current = {0}; XirHash *map = NULL;
    XrXirValueStatus status = hash_place_current(place, 0, admission, &current, &map);
    if (status != XR_XIR_VALUE_OK || !map->length) return status;
    if (!value_place_shared(place) && xir_object_unique(&map->object)) {
        status = hash_charge(admission, (uint64_t)map->length * 2 + (uint64_t)map->capacity * 3);
        if (status != XR_XIR_VALUE_OK) return status;
        XirObject *pending = NULL;
        for (size_t i = 0; i < map->length; ++i) hash_queue_entry(map, i, &pending);
        map->length = 0; map->order_count = 0;
        memset(map->data, 0, map->capacity * sizeof(uint64_t));
        memset(hash_order(map), 0, map->capacity * 2 * sizeof(uint64_t));
        release_pending(pending);
    } else {
        XirHash *copy = NULL;
        status = hash_allocate(map->object.type, 0, admission, &copy);
        if (status != XR_XIR_VALUE_OK) return status;
        XrXirValue replacement = hash_value(copy);
        status = value_place_publish(place, &replacement);
        if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&replacement); return status; }
        hash_publish_replacement(place, &replacement, &current, retired);
    }
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_hash_clear(const XrXirValuePlace *place, XrXirValueAdmission *admission) {
    if (!hash_admission_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = hash_clear(place, admission, NULL);
    xr_xir_value_graph_end(); return status;
}
XR_FUNC XrXirValueStatus xr_xir_hash_delete_deferred(const XrXirValuePlace *place, const XrXirValue *key,
    XrXirValueAdmission *admission, bool *removed, XrXirValue *retired) {
    if (!hash_admission_current(admission) || !hash_retired_valid(place, retired) || retired == key)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = hash_delete(place, key, admission, removed, retired);
    xr_xir_value_graph_end(); return status;
}
XR_FUNC XrXirValueStatus xr_xir_hash_clear_deferred(const XrXirValuePlace *place,
    XrXirValueAdmission *admission, XrXirValue *retired) {
    if (!hash_admission_current(admission) || !hash_retired_valid(place, retired))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = hash_clear(place, admission, retired);
    xr_xir_value_graph_end(); return status;
}
