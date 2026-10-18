/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_prepare.inc.c - Private typed hash backing and bounded probes
 */
typedef struct HashProbe { uint64_t hash; size_t bucket, entry; bool found; } HashProbe;
static bool hash_admission_current(const XrXirValueAdmission *admission) {
    return admission && admission->arena && admission->domain && admission_domain_current(admission);
}
static XrXirValue hash_value(XirHash *map) {
    XrXirValue value = {(uint32_t)map->object.type, 0, 0};
    memcpy(&value.payload, &map, sizeof(map)); return value;
}
static XrXirValueStatus hash_charge(XrXirValueAdmission *admission, uint64_t work) {
    if (work > admission->work) return XR_XIR_VALUE_LIMIT;
    admission->work -= work;
    return xr_xir_domain_work(admission->domain, work) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT;
}
static XrXirValueStatus hash_admit(const XrXirValue *value, uint32_t kind,
    XrXirValueAdmission *admission, XirHash **out) {
    if (!value || !hash_admission_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(admission->arena),
        (XrXirType)value->type);
    if (!node || (kind ? node->kind != kind :
        (node->kind != XR_XIR_TYPE_MAP && node->kind != XR_XIR_TYPE_SET))) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_admit(value, (XrXirType)value->type, admission);
    if (status == XR_XIR_VALUE_OK) *out = (XirHash *)object_pointer(value);
    return status;
}
static bool hash_capacity(size_t required, uint32_t stride, size_t *output) {
    if (!required) { *output = 0; return true; }
    size_t capacity = 8, bytes = 0;
    if (required > INT64_MAX) return false;
    while (required > capacity / 2) {
        if (capacity > SIZE_MAX / 2) return false;
        capacity *= 2;
    }
    if (!hash_data_size(capacity, stride, &bytes)) return false;
    *output = capacity; return true;
}
static XrXirValueStatus hash_allocate(XrXirType type, size_t capacity,
    XrXirValueAdmission *admission, XirHash **out) {
    HashLayout layout = {0};
    XrXirValueStatus status = hash_layout(admission->arena, type, &layout);
    if (status != XR_XIR_VALUE_OK) return status;
    uint64_t header = sizeof(XirHash) + (uint64_t)layout.release_capacity * sizeof(StorageFrame);
    size_t bytes = 0;
    if (header > SIZE_MAX || !hash_data_size(capacity, layout.stride, &bytes)) return XR_XIR_VALUE_LIMIT;
    status = hash_charge(admission, (uint64_t)capacity + 1);
    if (status != XR_XIR_VALUE_OK) return status;
    XirHash *map = (XirHash *)constructed_allocate(admission->domain,
        (XrXirTypeArena *)admission->arena, type, (size_t)header, &status);
    if (!map) return status;
    map->key_type = layout.key; map->value_type = layout.value;
    map->key_offset = layout.key_offset; map->value_offset = layout.value_offset;
    map->stride = layout.stride; map->release_capacity = layout.release_capacity;
    map->length = 0; map->capacity = capacity; map->data = NULL;
    if (bytes) {
        map->data = xr_xir_domain_allocate(admission->domain, bytes, &status);
        if (!map->data) { constructed_discard(&map->object, (size_t)header); return status; }
        memset(map->data, 0, capacity * sizeof(uint64_t));
    }
    xr_xir_value_object_publish(&map->object); *out = map;
    return XR_XIR_VALUE_OK;
}
static uint64_t hash_entry_hash(const XirHash *map, size_t index) {
    uint64_t hash = 0; memcpy(&hash, hash_entry(map, index), sizeof(hash)); return hash;
}
static XrXirValueStatus hash_probe(const XirHash *map, const XrXirValue *key,
    XrXirValueAdmission *admission, HashProbe *out) {
    if (!key || key->type != (uint32_t)map->key_type) return XR_XIR_VALUE_BAD_ARGUMENT;
    HashProbe probe = {0};
    XrXirValueStatus status = xr_xir_key_hash(key, admission, &probe.hash);
    if (status != XR_XIR_VALUE_OK) return status;
    if (!map->capacity) { *out = probe; return XR_XIR_VALUE_OK; }
    size_t mask = map->capacity - 1;
    probe.bucket = (size_t)probe.hash & mask;
    for (size_t count = 0; count < map->capacity; ++count) {
        status = hash_charge(admission, 1);
        if (status != XR_XIR_VALUE_OK) return status;
        uint64_t slot = hash_buckets(map)[probe.bucket];
        if (!slot) { *out = probe; return XR_XIR_VALUE_OK; }
        if (slot > map->length) return XR_XIR_VALUE_BAD_ARGUMENT;
        probe.entry = (size_t)slot - 1;
        if (hash_entry_hash(map, probe.entry) == probe.hash) {
            XrXirLayout layout = {0};
            XR_CHECK(xr_xir_compile_type_arena_layout(map->object.arena, map->key_type, &layout),
                "sealed hash keys retain their exact storage layout");
            XrXirValue stored={0};
            if (!storage_leaf_value(map->object.arena,map->key_type,
                hash_span(map,probe.entry,false).bytes,layout.size,&stored)) return XR_XIR_VALUE_BAD_ARGUMENT;
            status = xr_xir_key_equal(&stored, key, admission, &probe.found);
            if (status != XR_XIR_VALUE_OK) return status;
            if (probe.found) { *out = probe; return XR_XIR_VALUE_OK; }
        }
        probe.bucket = (probe.bucket + 1) & mask;
    }
    return XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirValueStatus hash_place_current(const XrXirValuePlace *place, uint32_t kind,
    XrXirValueAdmission *admission, XrXirValue *current, XirHash **map) {
    if (!place || !place->payload) return XR_XIR_VALUE_BAD_ARGUMENT;
    *current = (XrXirValue){(uint32_t)place->type, 0, 0};
    memcpy(&current->payload, place->payload, sizeof(current->payload));
    return hash_admit(current, kind, admission, map);
}
static XrXirValueStatus hash_prepare_field(StoragePrepared *prepared, const XrXirValue *value,
    XrXirType type, XrXirValueAdmission *admission) {
    XrXirLayout layout = {0};
    if (!xr_xir_value_argument(value,admission->arena,type) ||
        !xr_xir_compile_type_arena_layout(admission->arena, type, &layout)) return XR_XIR_VALUE_BAD_ARGUMENT;
    return storage_prepared_begin(prepared, type, value, layout.size, admission);
}
static void hash_transfer(StoragePrepared *prepared, XirHash *map, size_t index, bool value) {
    XR_CHECK(prepared->owns, "hash publication transfers exactly one prepared owner");
    if (prepared->stride) memcpy((void *)hash_span(map, index, value).bytes, prepared->bytes, prepared->stride);
    prepared->owns = false;
}
/* The destination is private. The only work here is charged before any root
 * changes, so table growth and cloning can fail without an undo log. */
static XrXirValueStatus hash_empty_bucket(const XirHash *map, uint64_t hash,
    XrXirValueAdmission *admission, size_t *out) {
    size_t mask = map->capacity - 1, bucket = (size_t)hash & mask;
    for (size_t count = 0; count < map->capacity; ++count) {
        XrXirValueStatus status = hash_charge(admission, 1);
        if (status != XR_XIR_VALUE_OK) return status;
        if (!hash_buckets(map)[bucket]) { *out = bucket; return XR_XIR_VALUE_OK; }
        bucket = (bucket + 1) & mask;
    }
    return XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirValueStatus hash_index_insert(XirHash *map, size_t index,
    XrXirValueAdmission *admission) {
    size_t bucket = 0;
    XrXirValueStatus status = hash_empty_bucket(map, hash_entry_hash(map, index), admission, &bucket);
    if (status == XR_XIR_VALUE_OK) hash_buckets(map)[bucket] = index + 1;
    return status;
}
