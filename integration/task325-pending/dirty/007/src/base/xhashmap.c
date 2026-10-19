/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhashmap.c - Transactional string table with one explicit resource policy
 */
#include "xhashmap.h"
#include "xio_policy.inc.h"
#include "xhash.h"
#include <string.h>

typedef union HashMapOwner {
    XrOsIoPolicy policy;
    long double alignment;
    void *pointer;
    uint64_t integer;
} HashMapOwner;
_Static_assert(sizeof(HashMapOwner) % _Alignof(XrHashMap) == 0, "Map owner preserves payload alignment");

static const XrOsIoPolicy *map_policy(const XrHashMap *map) {
    return &((const HashMapOwner *)map-1)->policy;
}
static bool empty(const XrHashMapEntry *entry) {
    return !entry->key && entry->value != XR_HASHMAP_TOMBSTONE;
}
static bool tombstone(const XrHashMapEntry *entry) {
    return !entry->key && entry->value == XR_HASHMAP_TOMBSTONE;
}
static bool hash_string(XrIoContext *io, const char *key, uint32_t *output) {
    size_t length;
    if (!io_length(io,key,&length) || !io_work(io,length)) return false;
    *output = xr_hash_bytes(key,length); return true;
}
static bool key_equal(XrIoContext *io, const char *first, const char *second) {
    for (size_t i = 0;; ++i) {
        if (!io_work(io,2)) return false;
        char a = first[i], b = second[i];
        if (a != b) return false;
        if (!a) return true;
    }
}
/* Only local outputs change during a probe; callers publish after status OK. */
static XrHashMapEntry *find_entry(XrIoContext *io, const XrHashMap *map,
    const char *key, uint32_t hash, uint32_t *index_out) {
    uint32_t start = hash & (map->capacity-1), deleted = UINT32_MAX;
    for (uint32_t i = 0; i < map->capacity; ++i) {
        if (!io_work(io,1)) return NULL;
        uint32_t index = (start+i) & (map->capacity-1);
        XrHashMapEntry *entry = &map->entries[index];
        if (empty(entry)) { *index_out = deleted != UINT32_MAX ? deleted : index; return NULL; }
        if (tombstone(entry)) { if (deleted == UINT32_MAX) deleted = index; }
        else if (entry->hash == hash && key_equal(io,entry->key,key)) { *index_out = index; return entry; }
        if (io->status != XR_OS_IO_OK) return NULL;
    }
    *index_out = deleted; return NULL;
}
static XrHashMapEntry *allocate_entries(XrIoContext *io, uint32_t capacity) {
    if (capacity && sizeof(XrHashMapEntry) > SIZE_MAX/(size_t)capacity) {
        io_status(io,XR_OS_IO_BUDGET); return NULL;
    }
    size_t bytes = (size_t)capacity*sizeof(XrHashMapEntry);
    XrHashMapEntry *entries = io_alloc(io,bytes);
    if (entries && !io_clear(io,entries,bytes)) { io_free(io,entries); return NULL; }
    return entries;
}
/* A private destination can be discarded at any failed work boundary. */
static bool rehash(XrIoContext *io, const XrHashMap *map, XrHashMapEntry *entries, uint32_t capacity) {
    for (uint32_t i = 0; i < map->capacity; ++i) {
        if (!io_work(io,1)) return false;
        const XrHashMapEntry *entry = &map->entries[i];
        if (!entry->key) continue;
        uint32_t index = entry->hash & (capacity-1);
        for (;;) {
            if (!io_work(io,1)) return false;
            if (!entries[index].key) break;
            index = (index+1) & (capacity-1);
        }
        if (!io_copy(io,&entries[index],entry,sizeof(*entry))) return false;
    }
    return true;
}
XR_FUNC XrOsIoStatus xr_hashmap_owned_new(const XrOsIoPolicy *policy, XrHashMap **output) {
    if (!io_policy_valid(policy) || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy,XR_OS_IO_OK};
    HashMapOwner *owner = io_alloc(&io,sizeof(*owner)+sizeof(XrHashMap));
    if (!owner) return io.status;
    owner->policy = *policy;
    XrHashMap *map = (void *)(owner+1);
    map->entries = allocate_entries(&io,16);
    if (!map->entries) { io_free(&io,owner); return io.status; }
    map->capacity = 16; map->count = 0; *output = map; return XR_OS_IO_OK;
}
XR_FUNC void xr_hashmap_owned_dispose(XrHashMap *map, XrHashMapIterFunc cleanup, void *userdata) {
    if (!map) return;
    XrOsIoPolicy policy = *map_policy(map);
    if (cleanup) for (uint32_t i = 0; i < map->capacity; ++i)
        if (map->entries[i].key) cleanup(map->entries[i].key,map->entries[i].value,userdata);
    policy.free(policy.context,map->entries);
    policy.free(policy.context,(HashMapOwner *)map-1);
}
XR_FUNC void xr_hashmap_owned_free(XrHashMap *map) { xr_hashmap_owned_dispose(map,NULL,NULL); }
XR_FUNC XrOsIoStatus xr_hashmap_owned_set(XrHashMap *map, const char *key, void *value) {
    if (!map || !key) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {map_policy(map),XR_OS_IO_OK};
    uint32_t hash, index = UINT32_MAX;
    if (!hash_string(&io,key,&hash)) return io.status;
    XrHashMapEntry *found = find_entry(&io,map,key,hash,&index);
    if (io.status != XR_OS_IO_OK) return io.status;
    if (found) {
        if (!io_work(&io,1)) return io.status;
        found->value = value; return XR_OS_IO_OK;
    }
    if (map->count >= map->capacity-map->capacity/4 || index == UINT32_MAX) {
        if (map->capacity >= UINT32_C(0x80000000)) return XR_OS_IO_BUDGET;
        uint32_t capacity = map->capacity*2;
        XrHashMapEntry *entries = allocate_entries(&io,capacity);
        if (!entries) return io.status;
        if (!rehash(&io,map,entries,capacity)) { io_free(&io,entries); return io.status; }
        index = hash & (capacity-1);
        for (;;) {
            if (!io_work(&io,1)) { io_free(&io,entries); return io.status; }
            if (!entries[index].key) break;
            index = (index+1) & (capacity-1);
        }
        if (!io_work(&io,1)) { io_free(&io,entries); return io.status; }
        entries[index] = (XrHashMapEntry){(char *)key,value,hash};
        XrHashMapEntry *previous = map->entries;
        map->entries = entries; map->capacity = capacity; ++map->count;
        io_free(&io,previous); return XR_OS_IO_OK;
    }
    if (!io_work(&io,1)) return io.status;
    map->entries[index] = (XrHashMapEntry){(char *)key,value,hash}; ++map->count;
    return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xr_hashmap_owned_get(const XrHashMap *map, const char *key, void **output) {
    if (!map || !key || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {map_policy(map),XR_OS_IO_OK}; uint32_t hash, index;
    if (!hash_string(&io,key,&hash)) return io.status;
    XrHashMapEntry *entry = find_entry(&io,map,key,hash,&index);
    if (io.status == XR_OS_IO_OK) *output = entry ? entry->value : NULL;
    return io.status;
}
XR_FUNC XrOsIoStatus xr_hashmap_owned_has(const XrHashMap *map, const char *key, bool *output) {
    if (!map || !key || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {map_policy(map),XR_OS_IO_OK}; uint32_t hash, index;
    if (!hash_string(&io,key,&hash)) return io.status;
    XrHashMapEntry *entry = find_entry(&io,map,key,hash,&index);
    if (io.status == XR_OS_IO_OK) *output = entry != NULL;
    return io.status;
}
XR_FUNC XrOsIoStatus xr_hashmap_owned_delete(XrHashMap *map, const char *key, bool *output) {
    if (!map || !key || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {map_policy(map),XR_OS_IO_OK}; uint32_t hash, index;
    if (!hash_string(&io,key,&hash)) return io.status;
    XrHashMapEntry *entry = find_entry(&io,map,key,hash,&index);
    if (io.status != XR_OS_IO_OK) return io.status;
    if (entry) {
        if (!io_work(&io,1)) return io.status;
        entry->key = NULL; entry->value = XR_HASHMAP_TOMBSTONE; --map->count;
    }
    *output = entry != NULL; return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xr_hashmap_owned_clear(XrHashMap *map) {
    if (!map) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {map_policy(map),XR_OS_IO_OK};
    if (io_clear(&io,map->entries,(size_t)map->capacity*sizeof(*map->entries))) map->count = 0;
    return io.status;
}
XR_FUNC XrOsIoStatus xr_hashmap_owned_foreach(const XrHashMap *map, XrHashMapIterFunc func, void *userdata) {
    if (!map || !func) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {map_policy(map),XR_OS_IO_OK};
    for (uint32_t i = 0; i < map->capacity; ++i) {
        if (!io_work(&io,1)) return io.status;
        if (map->entries[i].key) func(map->entries[i].key,map->entries[i].value,userdata);
    }
    return XR_OS_IO_OK;
}
