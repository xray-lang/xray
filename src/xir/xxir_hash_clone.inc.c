/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_clone.inc.c - Atomic dense backing relocation and shared copies
 */
typedef struct HashCloneEdit {
    size_t omit, replace;
    StoragePrepared *value;
} HashCloneEdit;
static XrXirValueStatus hash_clone(const XirHash *source, size_t capacity, const HashCloneEdit *edit,
    XrXirValueAdmission *admission, XirHash **out) {
    XirHash *copy = NULL;
    XrXirValueStatus status = hash_allocate(source->object.type, capacity, admission, &copy);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValue owned = hash_value(copy);
    StoragePack key = {0}, value = {0};
    status = storage_pack_begin(admission, source->key_type, &key);
    if (status == XR_XIR_VALUE_OK) status = storage_pack_begin(admission, source->value_type, &value);
    if (status == XR_XIR_VALUE_OK) {
        for (size_t i = 0; i < source->length; ++i) {
            if (edit && i == edit->omit) continue;
            status = hash_charge(admission, 1);
            if (status != XR_XIR_VALUE_OK) break;
            size_t index = copy->length;
            status = storage_pack_copy(&key, hash_span(source, i, false).bytes,
                (unsigned char *)hash_span(copy, index, false).bytes);
            if (status != XR_XIR_VALUE_OK) break;
            /* Replacing a value retains its original key position and never
             * acquires the discarded value, including a saturated old owner. */
            if (edit && i == edit->replace) hash_transfer(edit->value, copy, index, true);
            else if (source->value_type != XR_XIR_UNIT)
                status = storage_pack_copy(&value, hash_span(source, i, true).bytes,
                    (unsigned char *)hash_span(copy, index, true).bytes);
            if (status != XR_XIR_VALUE_OK) {
                storage_pack_release(&key, (unsigned char *)hash_span(copy, index, false).bytes, UINT64_MAX);
                break;
            }
            uint64_t hash = hash_entry_hash(source, i);
            memcpy(hash_entry(copy, index), &hash, sizeof(hash)); ++copy->length;
            status = hash_index_insert(copy, index, admission);
            if (status != XR_XIR_VALUE_OK) break;
        }
    }
    storage_pack_end(&value); storage_pack_end(&key);
    if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&owned); return status; }
    *out = copy; return XR_XIR_VALUE_OK;
}
/* Relocation copies borrowed bytes into a private backing. Its caller transfers
 * ownership only after all probes and work debits have succeeded. */
static XrXirValueStatus hash_relocate(const XirHash *source, XirHash *target,
    XrXirValueAdmission *admission) {
    XrXirValueStatus status = hash_charge(admission, source->length);
    if (status != XR_XIR_VALUE_OK) return status;
    for (size_t i = 0; i < source->length; ++i) {
        memcpy(hash_entry(target, i), hash_entry(source, i), source->stride);
        status = hash_index_insert(target, i, admission);
        if (status != XR_XIR_VALUE_OK) return status;
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus hash_grow(XirHash *map, size_t capacity, uint64_t hash,
    XrXirValueAdmission *admission, size_t *bucket) {
    size_t bytes = 0, previous = 0;
    if (!hash_data_size(capacity, map->stride, &bytes) ||
        !hash_data_size(map->capacity, map->stride, &previous)) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = hash_charge(admission, capacity);
    if (status != XR_XIR_VALUE_OK) return status;
    unsigned char *data = xr_xir_domain_allocate(admission->domain, bytes, &status);
    if (!data) return status;
    memset(data, 0, capacity * sizeof(uint64_t));
    XirHash backing = {.capacity = capacity, .stride = map->stride, .data = data};
    status = hash_relocate(map, &backing, admission);
    if (status == XR_XIR_VALUE_OK) status = hash_empty_bucket(&backing, hash, admission, bucket);
    if (status != XR_XIR_VALUE_OK) { xr_xir_domain_deallocate(admission->domain, data, bytes); return status; }
    unsigned char *old = map->data;
    map->data = data; map->capacity = capacity;
    if (old) xr_xir_domain_deallocate(admission->domain, old, previous);
    return XR_XIR_VALUE_OK;
}
