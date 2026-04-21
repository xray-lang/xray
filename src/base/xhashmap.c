/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhashmap.c - String hash table implementation
 */

#include "xhashmap.h"
#include "xmalloc.h"
#include "xarena.h"
#include "xchecks.h"
#include "xhash.h"
#include <string.h>

static uint32_t hash_string(const char *str) {
    return xr_hash_bytes(str, strlen(str));
}

// Empty slot: key==NULL && value!=TOMBSTONE
// Tombstone:  key==NULL && value==TOMBSTONE
static inline bool entry_is_empty(const XrHashMapEntry *e) {
    return e->key == NULL && e->value != XR_HASHMAP_TOMBSTONE;
}

static inline bool entry_is_tombstone(const XrHashMapEntry *e) {
    return e->key == NULL && e->value == XR_HASHMAP_TOMBSTONE;
}

static XrHashMapEntry* find_entry(XrHashMap *map, const char *key,
                                   uint32_t hash, uint32_t *out_index) {
    if (!map || !key) return NULL;

    uint32_t index = hash & (map->capacity - 1);

    XrHashMapEntry *tombstone = NULL;

    for (uint32_t i = 0; i < map->capacity; i++) {
        uint32_t probe_index = (index + i) & (map->capacity - 1);
        XrHashMapEntry *entry = &map->entries[probe_index];

        if (entry_is_empty(entry)) {
            if (out_index) {
                *out_index = tombstone ? (uint32_t)(tombstone - map->entries) : probe_index;
            }
            return NULL;
        } else if (entry_is_tombstone(entry)) {
            if (!tombstone) tombstone = entry;
        } else if (entry->hash == hash && strcmp(entry->key, key) == 0) {
            // Fast path: compare cached hash first, strcmp only on match
            if (out_index) *out_index = probe_index;
            return entry;
        }
    }

    if (out_index && tombstone) {
        *out_index = (uint32_t)(tombstone - map->entries);
    }
    return NULL;
}

static void resize(XrHashMap *map, uint32_t new_capacity) {
    if (map->is_arena_allocated) {
        XR_CHECK(false, "hashmap resize: arena-allocated map hit load factor, consider larger initial capacity");
        return;
    }

    XR_DCHECK(new_capacity >= map->capacity, "New capacity must be >= current capacity");
    XR_DCHECK((new_capacity & (new_capacity - 1)) == 0, "hashmap resize: capacity not power-of-2");

    XrHashMapEntry *new_entries = (XrHashMapEntry*)xr_malloc(
        sizeof(XrHashMapEntry) * new_capacity);
    if (!new_entries) return;

    memset(new_entries, 0, sizeof(XrHashMapEntry) * new_capacity);

    XrHashMapEntry *old_entries = map->entries;
    uint32_t old_capacity = map->capacity;

    map->entries = new_entries;
    map->capacity = new_capacity;
    map->count = 0;

    for (uint32_t i = 0; i < old_capacity; i++) {
        XrHashMapEntry *entry = &old_entries[i];
        if (entry->key != NULL) {
            // Reuse cached hash — skip strlen+FNV recomputation
            uint32_t h = entry->hash & (new_capacity - 1);
            while (new_entries[h].key != NULL) {
                h = (h + 1) & (new_capacity - 1);
            }
            new_entries[h].key = entry->key;
            new_entries[h].value = entry->value;
            new_entries[h].hash = entry->hash;
            map->count++;
        }
    }

    XR_DCHECK(map->count <= map->capacity, "hashmap resize: count > capacity after rehash");
    xr_free(old_entries);
}

XrHashMap* xr_hashmap_new(void) {
    XrHashMap *map = (XrHashMap*)xr_malloc(sizeof(XrHashMap));
    if (!map) return NULL;
    uint32_t initial_capacity = 16;
    map->entries = (XrHashMapEntry*)xr_malloc(sizeof(XrHashMapEntry) * initial_capacity);
    if (!map->entries) { xr_free(map); return NULL; }
    map->capacity = initial_capacity;
    map->count = 0;
    map->is_arena_allocated = false;

    memset(map->entries, 0, sizeof(XrHashMapEntry) * initial_capacity);
    return map;
}

XrHashMap* xr_hashmap_new_in_arena(XrArena *arena) {
    if (!arena) return NULL;

    XrHashMap *map = xr_arena_new(arena, XrHashMap);
    if (!map) return NULL;

    uint32_t initial_capacity = 16;
    map->entries = xr_arena_alloc(arena, sizeof(XrHashMapEntry) * initial_capacity);
    if (!map->entries) return NULL;

    map->capacity = initial_capacity;
    map->count = 0;
    map->is_arena_allocated = true;

    memset(map->entries, 0, sizeof(XrHashMapEntry) * initial_capacity);
    return map;
}

void xr_hashmap_free(XrHashMap *map) {
    if (!map) return;
    if (!map->is_arena_allocated && map->entries) xr_free(map->entries);
    if (!map->is_arena_allocated) xr_free(map);
}

void xr_hashmap_set(XrHashMap *map, const char *key, void *value) {
    XR_DCHECK(map != NULL, "Map must not be NULL");
    XR_DCHECK(key != NULL, "Key must not be NULL");

    // Load factor 75%: count * 4 >= capacity * 3
    if (map->count * 4 >= map->capacity * 3) {
        resize(map, map->capacity * XR_HASHMAP_GROW_FACTOR);
    }

    uint32_t hash = hash_string(key);
    uint32_t index;
    XrHashMapEntry *entry = find_entry(map, key, hash, &index);

    if (!entry) {
        entry = &map->entries[index];
        entry->key = (char*)key;
        entry->hash = hash;
        map->count++;
    }
    entry->value = value;
}

void* xr_hashmap_get(XrHashMap *map, const char *key) {
    if (!map || !key) return NULL;
    uint32_t hash = hash_string(key);
    uint32_t index;
    XrHashMapEntry *entry = find_entry(map, key, hash, &index);
    return entry ? entry->value : NULL;
}

bool xr_hashmap_has(XrHashMap *map, const char *key) {
    if (!map || !key) return false;
    uint32_t hash = hash_string(key);
    uint32_t index;
    XrHashMapEntry *entry = find_entry(map, key, hash, &index);
    return entry != NULL;
}

bool xr_hashmap_delete(XrHashMap *map, const char *key) {
    if (!map || !key) return false;
    uint32_t hash = hash_string(key);
    uint32_t index;
    XrHashMapEntry *entry = find_entry(map, key, hash, &index);
    if (!entry) return false;

    entry->key = NULL;
    entry->value = XR_HASHMAP_TOMBSTONE;
    // Keep hash non-zero — tombstone detection uses key==NULL && value==TOMBSTONE
    map->count--;
    return true;
}

void xr_hashmap_clear(XrHashMap *map) {
    if (!map) return;
    memset(map->entries, 0, sizeof(XrHashMapEntry) * map->capacity);
    map->count = 0;
}

void xr_hashmap_foreach(XrHashMap *map, XrHashMapIterFunc func, void *userdata) {
    if (!map || !func) return;
    for (uint32_t i = 0; i < map->capacity; i++) {
        XrHashMapEntry *entry = &map->entries[i];
        if (entry->key != NULL) {
            func(entry->key, entry->value, userdata);
        }
    }
}

