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
#include "layout.h"

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

static void mvl_header_init(XrMvlHeader *header, XrMvlDomain *domain,
                            const XrMvlTypeOps *ops, size_t bytes) {
    *header = (XrMvlHeader) {1, bytes, domain, ops, 1};
}

static void mvl_header_free(XrMvlHeader *header) {
    mvl_deallocate(header->domain, header, header->allocation_bytes);
}

static void mvl_array_destroy(XrMvlHeader *header) {
    XrMvlArray *array = (XrMvlArray *) header;
    mvl_deallocate(header->domain, array->data, array->capacity * sizeof(*array->data));
    mvl_header_free(header);
}

static void mvl_map_destroy(XrMvlHeader *header) {
    XrMvlMap *map = (XrMvlMap *) header;
    for (size_t i = 0; i < map->capacity; ++i)
        if (map->slots[i].key)
            xr_mvl_drop(&map->slots[i].key->header);
    mvl_deallocate(header->domain, map->slots, map->capacity * sizeof(*map->slots));
    mvl_header_free(header);
}

static const XrMvlTypeOps mvl_array_ops = {mvl_array_destroy, "Array<i64>"};
static const XrMvlTypeOps mvl_string_ops = {mvl_header_free, "String"};
static const XrMvlTypeOps mvl_map_ops = {mvl_map_destroy, "Map<String,i64>"};
static const XrMvlTypeOps mvl_pair_ops = {mvl_header_free, "{x:i64,y:i64}"};

XR_FUNCDEF XrMvlArray *xr_mvl_array_new(XrMvlDomain *domain) {
    XrMvlArray *array = mvl_allocate(domain, sizeof(*array));
    if (!array)
        return NULL;
    mvl_header_init(&array->header, domain, &mvl_array_ops, sizeof(*array));
    array->data = NULL;
    array->length = array->capacity = 0;
    return array;
}

static XrMvlArray *mvl_array_copy(const XrMvlArray *source, size_t capacity) {
    assert(capacity >= source->length && capacity <= SIZE_MAX / sizeof(int64_t));
    XrMvlArray *copy = xr_mvl_array_new(source->header.domain);
    if (!copy)
        return NULL;
    if (capacity) {
        copy->data = mvl_allocate(copy->header.domain, capacity * sizeof(*copy->data));
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

XR_FUNCDEF XrMvlStatus xr_mvl_array_push_slow(XrMvlArray **binding, int64_t value) {
    XrMvlArray *array = *binding;
    size_t capacity;
    if (array->length == SIZE_MAX ||
        !xr_mvl_capacity(array->capacity, array->length + 1, sizeof(int64_t), &capacity))
        return XR_MVL_OOM;
    if (array->header.refs != 1) {
        XrMvlArray *copy = mvl_array_copy(array, capacity);
        if (!copy)
            return XR_MVL_OOM;
        copy->data[copy->length++] = value;
        *binding = copy;
        xr_mvl_drop(&array->header);
        return XR_MVL_OK;
    }
    if (capacity != array->capacity) {
        int64_t *data = mvl_resize(array->header.domain, array->data,
                                  array->capacity * sizeof(*data), capacity * sizeof(*data));
        if (!data)
            return XR_MVL_OOM;
        array->data = data;
        array->capacity = capacity;
    }
    array->data[array->length++] = value;
    return XR_MVL_OK;
}

XR_FUNCDEF XrMvlStatus xr_mvl_array_set_slow(XrMvlArray **binding, size_t index, int64_t value) {
    XrMvlArray *array = *binding;
    if (index >= array->length)
        return XR_MVL_BOUNDS;
    XrMvlArray *copy = mvl_array_copy(array, array->capacity);
    if (!copy)
        return XR_MVL_OOM;
    copy->data[index] = value;
    *binding = copy;
    xr_mvl_drop(&array->header);
    return XR_MVL_OK;
}

XR_FUNCDEF XrMvlString *xr_mvl_string_new(XrMvlDomain *domain, const char *bytes, size_t length) {
    if ((!bytes && length) || length > SIZE_MAX - sizeof(XrMvlString) - 1)
        return NULL;
    size_t extent = sizeof(XrMvlString) + length + 1;
    XrMvlString *string = mvl_allocate(domain, extent);
    if (!string)
        return NULL;
    mvl_header_init(&string->header, domain, &mvl_string_ops, extent);
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
    mvl_header_init(&map->header, domain, &mvl_map_ops, sizeof(*map));
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
    if (!slot->key) {
        slot->hash = key->hash;
        slot->key = key;
        ++map->length;
    }
    slot->value = value;
}

static XrMvlMap *mvl_map_copy(const XrMvlMap *source, size_t capacity, XrMvlStatus *status) {
    XrMvlMap *copy = mvl_map_create(source->header.domain, capacity);
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
    XrMvlMapSlot *slots = mvl_allocate(map->header.domain, capacity * sizeof(*slots));
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
    mvl_deallocate(map->header.domain, map->slots, map->capacity * sizeof(*map->slots));
    map->slots = slots;
    map->capacity = capacity;
    return true;
}

XR_FUNCDEF XrMvlStatus xr_mvl_map_set_slow(XrMvlMap **binding, XrMvlString *key, int64_t value) {
    XrMvlMap *map = *binding;
    bool found = map->slots[xr_mvl_map_probe(map, key)].key != NULL;
    if (!found && !xr_mvl_retain(&key->header))
        return XR_MVL_RC_OVERFLOW;
    size_t capacity = map->capacity;
    if (!found && !xr_mvl_map_room(map->length, capacity) &&
        !xr_mvl_capacity(capacity, capacity + 1, sizeof(XrMvlMapSlot), &capacity)) {
        xr_mvl_drop(&key->header);
        return XR_MVL_OOM;
    }
    XrMvlStatus status = XR_MVL_OOM;
    if (map->header.refs != 1) {
        XrMvlMap *copy = mvl_map_copy(map, capacity, &status);
        if (!copy) {
            if (!found)
                xr_mvl_drop(&key->header);
            return status;
        }
        mvl_map_insert_prepared(copy, key, value);
        *binding = copy;
        xr_mvl_drop(&map->header);
    } else {
        if (capacity != map->capacity && !mvl_map_grow(map, capacity)) {
            if (!found)
                xr_mvl_drop(&key->header);
            return XR_MVL_OOM;
        }
        mvl_map_insert_prepared(map, key, value);
    }
    return XR_MVL_OK;
}

XR_FUNCDEF XrMvlPairObject *xr_mvl_pair_new(XrMvlDomain *domain, XrMvlPair value) {
    XrMvlPairObject *object = mvl_allocate(domain, sizeof(*object));
    if (!object)
        return NULL;
    mvl_header_init(&object->header, domain, &mvl_pair_ops, sizeof(*object));
    object->value = value;
    return object;
}
