/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * layout.h - Typed local managed objects with direct payload access
 *
 * KEY CONCEPT:
 *   Trusted typed pointers carry no compiler arena or execution-frame dependency.
 */
#ifndef XR_MVL_LAYOUT_H
#define XR_MVL_LAYOUT_H

#include "algorithms.h"

typedef enum {
    XR_MVL_OK,
    XR_MVL_OOM,
    XR_MVL_BOUNDS,
    XR_MVL_RC_OVERFLOW
} XrMvlStatus;

typedef struct {
    size_t live_blocks;
    size_t live_bytes;
    size_t allocations;
    size_t fail_after;
} XrMvlDomain;

typedef struct XrMvlHeader XrMvlHeader;
typedef struct {
    void (*destroy)(XrMvlHeader *);
    const char *name;
} XrMvlTypeOps;

struct XrMvlHeader {
    size_t refs;
    size_t allocation_bytes;
    XrMvlDomain *domain;
    const XrMvlTypeOps *ops;
    uint32_t state;
};

typedef struct {
    XrMvlHeader header;
    int64_t *data;
    size_t length;
    size_t capacity;
} XrMvlArray;

typedef struct {
    XrMvlHeader header;
    size_t length;
    uint64_t hash;
    char bytes[];
} XrMvlString;

typedef struct {
    uint64_t hash;
    XrMvlString *key;
    int64_t value;
} XrMvlMapSlot;

typedef struct {
    XrMvlHeader header;
    XrMvlMapSlot *slots;
    size_t length;
    size_t capacity;
} XrMvlMap;

typedef struct {
    int64_t x;
    int64_t y;
} XrMvlPair;

typedef struct {
    XrMvlHeader header;
    XrMvlPair value;
} XrMvlPairObject;

static inline void xr_mvl_domain_init(XrMvlDomain *domain) {
    *domain = (XrMvlDomain) {0, 0, 0, SIZE_MAX};
}

static inline bool xr_mvl_retain(XrMvlHeader *header) {
    assert(header && header->refs && header->state == 1);
    if (header->refs == SIZE_MAX)
        return false;
    ++header->refs;
    return true;
}

static inline void xr_mvl_drop(XrMvlHeader *header) {
    if (!header)
        return;
    assert(header->refs && header->state == 1);
    if (--header->refs == 0)
        header->ops->destroy(header);
}

XR_FUNC XrMvlArray *xr_mvl_array_new(XrMvlDomain *domain);
XR_FUNC XrMvlStatus xr_mvl_array_push_slow(XrMvlArray **binding, int64_t value);
XR_FUNC XrMvlStatus xr_mvl_array_set_slow(XrMvlArray **binding, size_t index, int64_t value);
XR_FUNC XrMvlString *xr_mvl_string_new(XrMvlDomain *domain, const char *bytes, size_t length);
XR_FUNC XrMvlMap *xr_mvl_map_new(XrMvlDomain *domain);
XR_FUNC XrMvlStatus xr_mvl_map_set_slow(XrMvlMap **binding, XrMvlString *key, int64_t value);
XR_FUNC XrMvlPairObject *xr_mvl_pair_new(XrMvlDomain *domain, XrMvlPair value);
XR_FUNC bool xr_mvl_pair_escape(XrMvlPairObject *value);
XR_FUNC XrMvlPairObject *xr_mvl_pair_escaped(void);
XR_FUNC void xr_mvl_pair_escape_clear(void);
XR_FUNC bool xr_mvl_verify(void);

static inline XrMvlStatus xr_mvl_array_push(XrMvlArray **binding, int64_t value) {
    XrMvlArray *array = *binding;
    if (array->header.refs == 1 && array->length < array->capacity) {
        array->data[array->length++] = value;
        return XR_MVL_OK;
    }
    return xr_mvl_array_push_slow(binding, value);
}

static inline XrMvlStatus xr_mvl_array_set(XrMvlArray **binding, size_t index, int64_t value) {
    XrMvlArray *array = *binding;
    if (index >= array->length)
        return XR_MVL_BOUNDS;
    if (array->header.refs != 1)
        return xr_mvl_array_set_slow(binding, index, value);
    array->data[index] = value;
    return XR_MVL_OK;
}

static inline size_t xr_mvl_map_probe(const XrMvlMap *map, const XrMvlString *key) {
    size_t index = (size_t) key->hash & (map->capacity - 1);
    while (map->slots[index].key) {
        const XrMvlMapSlot *slot = &map->slots[index];
        if (slot->hash == key->hash && slot->key->length == key->length &&
            memcmp(slot->key->bytes, key->bytes, key->length) == 0)
            break;
        index = (index + 1) & (map->capacity - 1);
    }
    return index;
}

static inline bool xr_mvl_map_get(const XrMvlMap *map, const XrMvlString *key, int64_t *value) {
    const XrMvlMapSlot *slot = &map->slots[xr_mvl_map_probe(map, key)];
    if (!slot->key)
        return false;
    *value = slot->value;
    return true;
}

static inline XrMvlStatus xr_mvl_map_set(XrMvlMap **binding, XrMvlString *key, int64_t value) {
    XrMvlMap *map = *binding;
    XrMvlMapSlot *slot = &map->slots[xr_mvl_map_probe(map, key)];
    if (map->header.refs != 1 ||
        (!slot->key && !xr_mvl_map_room(map->length, map->capacity)))
        return xr_mvl_map_set_slow(binding, key, value);
    if (!slot->key) {
        if (!xr_mvl_retain(&key->header))
            return XR_MVL_RC_OVERFLOW;
        slot->hash = key->hash;
        slot->key = key;
        ++map->length;
    }
    slot->value = value;
    return XR_MVL_OK;
}

#endif // XR_MVL_LAYOUT_H
