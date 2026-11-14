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
    assert(bytes && old_bytes <= domain->live_bytes);
    if (domain->fail_after == 0 || bytes > SIZE_MAX - (domain->live_bytes - old_bytes))
        return NULL;
    if (domain->fail_after != SIZE_MAX)
        --domain->fail_after;
    bool had_memory = memory != NULL;
    void *replacement = xr_realloc(memory, bytes);
    if (replacement) {
        if (!had_memory)
            ++domain->live_blocks;
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

static void mvl_array_destroy(XrMvlHeader *header) {
    XrMvlArray *array = (XrMvlArray *) header;
    mvl_deallocate(header->ops->domain, array->data, array->capacity * sizeof(*array->data));
    mvl_header_free(header, sizeof(*array));
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

XR_FUNCDEF XrMvlArray *xr_mvl_array_new(XrMvlDomain *domain) {
    XrMvlArray *array = mvl_allocate(domain, sizeof(*array));
    if (!array)
        return NULL;
    mvl_header_init(&array->header, &domain->array_ops);
    array->data = NULL;
    array->length = array->capacity = 0;
    return array;
}

static XrMvlArray *mvl_array_copy(const XrMvlArray *source, size_t capacity) {
    assert(capacity >= source->length && capacity <= SIZE_MAX / sizeof(int64_t));
    XrMvlArray *copy = xr_mvl_array_new(source->header.ops->domain);
    if (!copy)
        return NULL;
    if (capacity) {
        copy->data = mvl_allocate(copy->header.ops->domain, capacity * sizeof(*copy->data));
        if (!copy->data) {
            xr_mvl_drop(&copy->header);
            return NULL;
        }
        copy->capacity = capacity;
    }
    if (source->length)
        memcpy(copy->data, source->data, source->length * sizeof(*copy->data));
    copy->length = source->length;
    return copy;
}

XR_FUNCDEF XrMvlArrayWrite xr_mvl_array_access_write(XrMvlArrayAccess access,
    size_t index, int64_t value, bool append) {
    XrMvlArray *array = access.owner;
    XrMvlArrayWrite result = {XR_MVL_INVALID_OWNER, xr_mvl_array_access_start(array), NULL};
    if (!array)
        return result;
    if (!append && index >= array->length) {
        result.status = XR_MVL_BOUNDS;
        return result;
    }
    size_t capacity = array->capacity;
    result.status = XR_MVL_OOM;
    if (append && (array->length == SIZE_MAX ||
        !xr_mvl_capacity(capacity, array->length + 1, sizeof(int64_t), &capacity)))
        return result;
    XrMvlArray *target = array;
    if (!access.writable && !xr_signed_rc_unique(&array->header.refs)) {
        target = mvl_array_copy(array, capacity);
        if (!target)
            return result;
        result.previous = array;
    } else if (capacity != array->capacity) {
        int64_t *data = mvl_resize(array->header.ops->domain, array->data,
                                  array->capacity * sizeof(*data), capacity * sizeof(*data));
        if (!data)
            return result;
        array->data = data;
        array->capacity = capacity;
    }
    if (append)
        index = target->length;
    target->data[index] = value;
    if (append)
        ++target->length;
    result.access = (XrMvlArrayAccess) {target, target->data, target->length, target->capacity, true};
    result.status = XR_MVL_OK;
    return result;
}

XR_FUNCDEF XrMvlStatus xr_mvl_array_push_slow(XrMvlArray **binding, int64_t value) {
    XrMvlArrayWrite result = xr_mvl_array_access_write(xr_mvl_array_access_start(*binding), 0, value, true);
    if (result.status == XR_MVL_OK) {
        *binding = result.access.owner;
        if (result.previous) xr_mvl_drop(&result.previous->header);
    }
    return result.status;
}

XR_FUNCDEF XrMvlStatus xr_mvl_array_set_slow(XrMvlArray **binding, size_t index, int64_t value) {
    XrMvlArrayWrite result = xr_mvl_array_access_write(xr_mvl_array_access_start(*binding), index, value, false);
    if (result.status == XR_MVL_OK) {
        *binding = result.access.owner;
        if (result.previous) xr_mvl_drop(&result.previous->header);
    }
    return result.status;
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
