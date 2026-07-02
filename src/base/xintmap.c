/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xintmap.c - Integer-keyed hash table implementation
 */

#include "xintmap.h"
#include "xchecks.h"
#include "xmalloc.h"
#include <stdlib.h>
#include <string.h>

// Hash function for uint32_t keys (uses FNV-1a mixing)
static inline uint32_t hash_key(uint32_t key) {
    // Multiply-shift hash for integers
    // Good distribution for sequential and sparse keys
    key = ((key >> 16) ^ key) * 0x45d9f3b;
    key = ((key >> 16) ^ key) * 0x45d9f3b;
    key = (key >> 16) ^ key;
    return key;
}

// Find slot for key (returns index, or capacity if not found for get)
static uint32_t find_slot(XrIntMap *map, uint32_t key, bool for_insert) {
    uint32_t mask = map->capacity - 1;
    uint32_t index = hash_key(key) & mask;
    uint32_t first_tombstone = UINT32_MAX;

    for (uint32_t i = 0; i < map->capacity; i++) {
        uint32_t probe = (index + i) & mask;
        XrIntMapEntry *entry = &map->entries[probe];

        if (entry->key == XR_INTMAP_EMPTY) {
            // Empty slot: return tombstone if found, else this slot
            return (for_insert && first_tombstone != UINT32_MAX) ? first_tombstone : probe;
        }

        if (entry->key == XR_INTMAP_TOMBSTONE) {
            // Remember first tombstone for insertion
            if (first_tombstone == UINT32_MAX) {
                first_tombstone = probe;
            }
            continue;
        }

        if (entry->key == key) {
            return probe;  // Found existing key
        }
    }

    // Table is full (shouldn't happen with proper load factor)
    return for_insert && first_tombstone != UINT32_MAX ? first_tombstone : map->capacity;
}

// Resize map to new capacity. On allocation failure the old table is
// kept: the map keeps working above its load factor (degraded probing)
// and xr_intmap_set reports failure only once no free slot is left.
static void resize(XrIntMap *map, uint32_t new_capacity) {
    XR_DCHECK((new_capacity & (new_capacity - 1)) == 0, "intmap resize: capacity not power-of-2");
    XrIntMapEntry *old_entries = map->entries;
    uint32_t old_capacity = map->capacity;

    map->entries = (XrIntMapEntry *) xr_malloc(new_capacity * sizeof(XrIntMapEntry));
    if (!map->entries) {
        map->entries = old_entries;
        return;
    }

    // Mark all new slots as empty
    for (uint32_t i = 0; i < new_capacity; i++) {
        map->entries[i].key = XR_INTMAP_EMPTY;
        map->entries[i].value = NULL;
    }

    map->capacity = new_capacity;
    map->count = 0;
    map->tombstones = 0;

    // Rehash old entries (cannot fail: the doubled table always has room)
    for (uint32_t i = 0; i < old_capacity; i++) {
        if (old_entries[i].key != XR_INTMAP_EMPTY && old_entries[i].key != XR_INTMAP_TOMBSTONE) {
            (void) xr_intmap_set(map, old_entries[i].key, old_entries[i].value);
        }
    }

    xr_free(old_entries);
}

XrIntMap *xr_intmap_new(void) {
    XrIntMap *map = (XrIntMap *) xr_malloc(sizeof(XrIntMap));
    if (!map)
        return NULL;

    map->capacity = XR_INTMAP_MIN_CAPACITY;
    map->count = 0;
    map->tombstones = 0;

    map->entries = (XrIntMapEntry *) xr_malloc(map->capacity * sizeof(XrIntMapEntry));
    if (!map->entries) {
        xr_free(map);
        return NULL;
    }

    // Mark all slots as empty
    for (uint32_t i = 0; i < map->capacity; i++) {
        map->entries[i].key = XR_INTMAP_EMPTY;
        map->entries[i].value = NULL;
    }

    return map;
}

void xr_intmap_free(XrIntMap *map) {
    if (!map)
        return;
    xr_free(map->entries);
    xr_free(map);
}

bool xr_intmap_set(XrIntMap *map, uint32_t key, void *value) {
    if (!map)
        return false;

    // Reserved sentinel values can never be stored
    XR_DCHECK(key != XR_INTMAP_EMPTY && key != XR_INTMAP_TOMBSTONE,
              "intmap set: reserved sentinel key");
    if (key == XR_INTMAP_EMPTY || key == XR_INTMAP_TOMBSTONE) {
        return false;
    }

    // Check load factor and resize if needed
    uint32_t load = map->count + map->tombstones;
    if (load * 100 >= map->capacity * 75) {
        resize(map, map->capacity * XR_INTMAP_GROW_FACTOR);
    }

    uint32_t index = find_slot(map, key, true);
    if (index >= map->capacity) {
        // Table full and key absent: resize already failed under OOM.
        // Report failure instead of silently dropping the insert.
        return false;
    }

    XrIntMapEntry *entry = &map->entries[index];
    bool is_new = (entry->key == XR_INTMAP_EMPTY || entry->key == XR_INTMAP_TOMBSTONE);
    bool was_tombstone = (entry->key == XR_INTMAP_TOMBSTONE);

    entry->key = key;
    entry->value = value;

    if (is_new) {
        map->count++;
        if (was_tombstone) {
            map->tombstones--;
        }
    }
    XR_DCHECK(map->count <= map->capacity, "intmap set: count > capacity");
    return true;
}

void *xr_intmap_get(XrIntMap *map, uint32_t key) {
    if (!map)
        return NULL;
    if (key == XR_INTMAP_EMPTY || key == XR_INTMAP_TOMBSTONE)
        return NULL;

    uint32_t index = find_slot(map, key, false);
    if (index >= map->capacity)
        return NULL;

    XrIntMapEntry *entry = &map->entries[index];
    if (entry->key == key) {
        return entry->value;
    }
    return NULL;
}

bool xr_intmap_has(XrIntMap *map, uint32_t key) {
    if (!map)
        return false;
    if (key == XR_INTMAP_EMPTY || key == XR_INTMAP_TOMBSTONE)
        return false;

    uint32_t index = find_slot(map, key, false);
    if (index >= map->capacity)
        return false;

    return map->entries[index].key == key;
}

bool xr_intmap_delete(XrIntMap *map, uint32_t key) {
    if (!map)
        return false;
    if (key == XR_INTMAP_EMPTY || key == XR_INTMAP_TOMBSTONE)
        return false;

    uint32_t index = find_slot(map, key, false);
    if (index >= map->capacity)
        return false;

    XrIntMapEntry *entry = &map->entries[index];
    if (entry->key != key)
        return false;

    // Mark as tombstone
    entry->key = XR_INTMAP_TOMBSTONE;
    entry->value = NULL;
    map->count--;
    map->tombstones++;

    return true;
}

void xr_intmap_clear(XrIntMap *map) {
    if (!map)
        return;

    for (uint32_t i = 0; i < map->capacity; i++) {
        map->entries[i].key = XR_INTMAP_EMPTY;
        map->entries[i].value = NULL;
    }
    map->count = 0;
    map->tombstones = 0;
}

void xr_intmap_foreach(XrIntMap *map, XrIntMapIterFunc func, void *userdata) {
    if (!map || !func)
        return;

    for (uint32_t i = 0; i < map->capacity; i++) {
        XrIntMapEntry *entry = &map->entries[i];
        if (entry->key != XR_INTMAP_EMPTY && entry->key != XR_INTMAP_TOMBSTONE) {
            func(entry->key, entry->value, userdata);
        }
    }
}
