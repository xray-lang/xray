/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * layout.c - Fallible preparation and final destruction for typed objects
 *
 * KEY CONCEPT:
 *   Allocation and COW are cold; a prepared replacement commits without failure.
 */
#include "access.h"

static void *mvl_allocate(XrMvlDomain *domain, size_t bytes) {
    assert(domain && bytes);
    if (domain->fail_after == 0 || bytes > SIZE_MAX - domain->live_bytes)
        return NULL;
    if (domain->fail_after != SIZE_MAX)
        --domain->fail_after;
    void *memory = xr_malloc(bytes);
    if (memory) {
        ++domain->live_blocks;
        domain->live_bytes += bytes;
        ++domain->allocations;
    }
    return memory;
}

static void mvl_deallocate(XrMvlDomain *domain, void *memory, size_t bytes) {
    if (!memory)
        return;
    assert(domain->live_blocks && bytes <= domain->live_bytes);
    --domain->live_blocks;
    domain->live_bytes -= bytes;
    xr_free(memory);
}

static void *mvl_resize(XrMvlDomain *domain, void *memory, size_t old_bytes, size_t bytes) {
    assert(memory && bytes && old_bytes <= domain->live_bytes);
    if (domain->fail_after == 0 || bytes > SIZE_MAX - (domain->live_bytes - old_bytes))
        return NULL;
    if (domain->fail_after != SIZE_MAX)
        --domain->fail_after;
    void *replacement = xr_realloc(memory, bytes);
    if (replacement) {
        domain->live_bytes = domain->live_bytes - old_bytes + bytes;
        ++domain->allocations;
    }
    return replacement;
}

static void mvl_header_init(XrMvlHeader *header, const XrMvlTypeOps *ops) {
    xr_signed_rc_initialize(&header->refs);
    header->flags = 0;
    header->ops = ops;
}

static void mvl_header_free(XrMvlHeader *header, size_t bytes) {
    mvl_deallocate(header->ops->domain, header, bytes);
}

static void mvl_string_destroy(XrMvlHeader *header) {
    XrMvlString *string = (XrMvlString *)header;
    mvl_header_free(header, sizeof(*string) + string->length + 1);
}

XR_FUNCDEF void xr_mvl_string_dispose_last(XrMvlString *string) {
    mvl_string_destroy(&string->header);
}

static const size_t mvl_array_maximum = (SIZE_MAX - sizeof(XrMvlArrayBacking)) / sizeof(int64_t);

static size_t mvl_array_extent(size_t capacity) {
    assert(capacity <= mvl_array_maximum);
    return sizeof(XrMvlArrayBacking) + capacity * sizeof(int64_t);
}

static void mvl_array_destroy(XrMvlHeader *header) {
    XrMvlArrayBacking *array = (XrMvlArrayBacking *) header;
    mvl_header_free(header, mvl_array_extent(array->capacity));
}

static void mvl_map_destroy(XrMvlHeader *header) {
    XrMvlMap *map = (XrMvlMap *) header;
    for (size_t i = 0; i < map->capacity; ++i)
        if (map->slots[i].key)
            xr_mvl_drop(&map->slots[i].key->header);
    mvl_deallocate(header->ops->domain, map->slots, map->capacity * sizeof(*map->slots));
    mvl_header_free(header, sizeof(*map));
}

static void mvl_pair_destroy(XrMvlHeader *header) {
    mvl_header_free(header, sizeof(XrMvlPairObject));
}

XR_FUNCDEF void xr_mvl_domain_init(XrMvlDomain *domain) {
    *domain = (XrMvlDomain){.fail_after=SIZE_MAX,
        .array_ops={mvl_array_destroy, "Array<i64>", domain},
        .string_ops={mvl_string_destroy, "String", domain},
        .map_ops={mvl_map_destroy, "Map<String,i64>", domain},
        .pair_ops={mvl_pair_destroy, "{x:i64,y:i64}", domain}};
}

static XrMvlArrayBacking *mvl_array_create(XrMvlDomain *domain, size_t capacity) {
    assert(capacity <= mvl_array_maximum);
    XrMvlArrayBacking *array = mvl_allocate(domain, mvl_array_extent(capacity));
    if (!array)
        return NULL;
    mvl_header_init(&array->header, &domain->array_ops);
    array->reserved = 0;
    array->capacity = capacity;
    return array;
}

XR_FUNCDEF XrMvlArray xr_mvl_array_new(XrMvlDomain *domain) {
    return (XrMvlArray){mvl_array_create(domain, 0), 0};
}

static XrMvlArrayBacking *mvl_array_copy(const XrMvlArrayBacking *source, size_t length,
    size_t capacity) {
    assert(capacity >= length && capacity <= mvl_array_maximum);
    XrMvlArrayBacking *copy = mvl_array_create(source->header.ops->domain, capacity);
    if (!copy)
        return NULL;
    if (length)
        memcpy(copy->data, source->data, length * sizeof(*copy->data));
    return copy;
}

XR_FUNCDEF XrMvlArrayMutation xr_mvl_array_mutate(XrMvlArrayBacking *array, size_t length,
    size_t index, int64_t value, bool append, bool writable) {
    if (!array || length > array->capacity)
        return (XrMvlArrayMutation){XR_MVL_INVALID_OWNER, NULL, NULL};
    if (!append && index >= length)
        return (XrMvlArrayMutation){XR_MVL_BOUNDS, NULL, NULL};
    size_t capacity = array->capacity;
    const XrMvlArrayMutation oom = {XR_MVL_OOM, NULL, NULL};
    if (append && (length >= mvl_array_maximum ||
        !xr_mvl_capacity(capacity, length + 1, sizeof(int64_t), &capacity) || capacity > mvl_array_maximum))
        return oom;
    bool grows = capacity != array->capacity;
    bool unique = (writable && !grows) || xr_signed_rc_unique(&array->header.refs);
    XrMvlArrayBacking *target = array, *previous = NULL;
    if (!unique || (grows && array->header.flags)) {
        target = mvl_array_copy(array, length, capacity);
        if (!target)
            return oom;
        previous = array;
    } else if (grows) {
        /* The caller supplies an exclusive window. The built-in resize may
         * consume the old allocation, including its header and cached data. */
        XrMvlDomain *domain = array->header.ops->domain;
        size_t old_bytes = mvl_array_extent(array->capacity);
        target = mvl_resize(domain, array, old_bytes, mvl_array_extent(capacity));
        if (!target)
            return oom;
        target->capacity = capacity;
    }
    if (append)
        index = length;
    target->data[index] = value;
    return (XrMvlArrayMutation){XR_MVL_OK, target, previous};
}

XR_FUNCDEF XrMvlArrayWrite xr_mvl_array_access_write(XrMvlArrayAccess access,
    size_t index, int64_t value, bool append) {
    size_t length = access.value.length;
    XrMvlArrayMutation mutation = xr_mvl_array_mutate(access.value.owner, length, index, value,
        append, access.writable);
    if (mutation.status != XR_MVL_OK)
        return (XrMvlArrayWrite){mutation.status, xr_mvl_array_access_start(access.value), NULL};
    XrMvlArrayBacking *owner = mutation.owner;
    XrMvlArrayAccess updated = {{owner, length + append}, owner->data, owner->capacity, true};
    return (XrMvlArrayWrite){XR_MVL_OK, updated, mutation.previous};
}

XR_FUNCDEF XrMvlString *xr_mvl_string_new(XrMvlDomain *domain, const char *bytes, size_t length) {
    if ((!bytes && length) || length > SIZE_MAX - sizeof(XrMvlString) - 1)
        return NULL;
    size_t extent = sizeof(XrMvlString) + length + 1;
    XrMvlString *string = mvl_allocate(domain, extent);
    if (!string)
        return NULL;
    mvl_header_init(&string->header, &domain->string_ops);
    string->length = length;
    string->hash = xr_hash_bytes64(bytes, length);
    if (length)
        memcpy(string->bytes, bytes, length);
    string->bytes[length] = '\0';
    return string;
}

static XrMvlMap *mvl_map_create(XrMvlDomain *domain, size_t capacity) {
    assert(capacity >= 8 && !(capacity & (capacity - 1)));
    if (capacity > SIZE_MAX / sizeof(XrMvlMapSlot))
        return NULL;
    XrMvlMap *map = mvl_allocate(domain, sizeof(*map));
    if (!map)
        return NULL;
    mvl_header_init(&map->header, &domain->map_ops);
    map->slots = mvl_allocate(domain, capacity * sizeof(*map->slots));
    map->length = map->capacity = 0;
    if (!map->slots) {
        xr_mvl_drop(&map->header);
        return NULL;
    }
    memset(map->slots, 0, capacity * sizeof(*map->slots));
    map->capacity = capacity;
    return map;
}

XR_FUNCDEF XrMvlMap *xr_mvl_map_new(XrMvlDomain *domain) {
    return mvl_map_create(domain, 8);
}

static void mvl_map_insert_prepared(XrMvlMap *map, XrMvlString *key, int64_t value) {
    XrMvlMapSlot *slot = &map->slots[xr_mvl_map_probe(map, key)];
    xr_mvl_map_commit_slot(map, slot, key, value);
}

static XrMvlMap *mvl_map_copy(const XrMvlMap *source, size_t capacity, XrMvlStatus *status) {
    XrMvlMap *copy = mvl_map_create(source->header.ops->domain, capacity);
    if (!copy)
        return NULL;
    for (size_t i = 0; i < source->capacity; ++i) {
        const XrMvlMapSlot *slot = &source->slots[i];
        if (!slot->key)
            continue;
        if (!xr_mvl_retain(&slot->key->header)) {
            *status = XR_MVL_RC_OVERFLOW;
            xr_mvl_drop(&copy->header);
            return NULL;
        }
        mvl_map_insert_prepared(copy, slot->key, slot->value);
    }
    return copy;
}

static bool mvl_map_grow(XrMvlMap *map, size_t capacity) {
    XrMvlMapSlot *slots = mvl_allocate(map->header.ops->domain, capacity * sizeof(*slots));
    if (!slots)
        return false;
    memset(slots, 0, capacity * sizeof(*slots));
    for (size_t i = 0; i < map->capacity; ++i) {
        if (!map->slots[i].key)
            continue;
        size_t index = (size_t) map->slots[i].hash & (capacity - 1);
        while (slots[index].key)
            index = (index + 1) & (capacity - 1);
        slots[index] = map->slots[i];
    }
    mvl_deallocate(map->header.ops->domain, map->slots, map->capacity * sizeof(*map->slots));
    map->slots = slots;
    map->capacity = capacity;
    return true;
}

static XrMvlStatus mvl_map_set_slow(XrMvlMap **binding, XrMvlString *key,
                                   int64_t value, XrMvlString **key_owner) {
    XrMvlMap *map = *binding;
    bool found = map->slots[xr_mvl_map_probe(map, key)].key != NULL;
    bool copied_key = !found && !key_owner;
    if (copied_key && !xr_mvl_retain(&key->header))
        return XR_MVL_RC_OVERFLOW;
    size_t capacity = map->capacity;
    if (!found && !xr_mvl_map_room(map->length, capacity) &&
        !xr_mvl_capacity(capacity, capacity + 1, sizeof(XrMvlMapSlot), &capacity)) {
        if (copied_key)
            xr_mvl_drop(&key->header);
        return XR_MVL_OOM;
    }
    XrMvlStatus status = XR_MVL_OOM;
    XrMvlMap *retired = NULL;
    if (!xr_signed_rc_unique(&map->header.refs)) {
        XrMvlMap *copy = mvl_map_copy(map, capacity, &status);
        if (!copy) {
            if (copied_key)
                xr_mvl_drop(&key->header);
            return status;
        }
        mvl_map_insert_prepared(copy, key, value);
        *binding = copy;
        retired = map;
    } else {
        if (capacity != map->capacity && !mvl_map_grow(map, capacity)) {
            if (copied_key)
                xr_mvl_drop(&key->header);
            return XR_MVL_OOM;
        }
        mvl_map_insert_prepared(map, key, value);
    }
    if (key_owner) {
        *key_owner = NULL;
        if (found)
            xr_mvl_drop(&key->header);
    }
    if (retired)
        xr_mvl_drop(&retired->header);
    return XR_MVL_OK;
}

XR_FUNCDEF XrMvlStatus xr_mvl_map_set_slow(XrMvlMap **binding, XrMvlString *key, int64_t value) {
    return mvl_map_set_slow(binding, key, value, NULL);
}

XR_FUNCDEF XrMvlStatus xr_mvl_map_set_take_slow(XrMvlMap **binding,
                                             XrMvlString **key_owner, int64_t value) {
    if (!binding || !*binding || !key_owner || !*key_owner)
        return XR_MVL_INVALID_OWNER;
    return mvl_map_set_slow(binding, *key_owner, value, key_owner);
}

XR_FUNCDEF XrMvlPairObject *xr_mvl_pair_new(XrMvlDomain *domain, XrMvlPair value) {
    XrMvlPairObject *object = mvl_allocate(domain, sizeof(*object));
    if (!object)
        return NULL;
    mvl_header_init(&object->header, &domain->pair_ops);
    object->value = value;
    return object;
}
