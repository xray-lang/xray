/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhashmap.h - String-keyed hash table for compiler use
 *
 * KEY CONCEPT:
 *   Simple string->void* hash table using open addressing + linear probing.
 *   Designed for compiler symbol tables, NOT for runtime Map type.
 *
 * VS xhash.h:
 *   - xhash.h: Hash functions for XrValue keys (runtime Map/Set)
 *   - xhashmap.h: Hash table for C strings (compiler internal)
 *
 * MEMORY MODES:
 *   - xr_hashmap_new(): Uses malloc, caller must free
 *   - xr_hashmap_new_in_arena(): Uses Arena, freed with arena
 *
 * USE CASES:
 *   - Compiler symbol tables (variable names -> Local*)
 *   - Module registry (path -> XrModule*)
 *   - Class field lookup (name -> field index)
 *
 * RELATED MODULES:
 *   - xhash.h: Value hashing for runtime Map/Set
 *   - xmap.h: Runtime Map object (uses xhash.h)
 */

#ifndef XHASHMAP_H
#define XHASHMAP_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "xdefs.h"

// Tombstone sentinel: key==NULL && value==XR_HASHMAP_TOMBSTONE
#define XR_HASHMAP_TOMBSTONE ((void*)(uintptr_t)1)

typedef struct XrHashMapEntry {
    char *key;
    void *value;
} XrHashMapEntry;

typedef struct XrHashMap {
    XrHashMapEntry *entries;
    uint32_t capacity;
    uint32_t count;
    bool is_arena_allocated;
} XrHashMap;

XR_FUNC XrHashMap* xr_hashmap_new(void);

struct XrArena;
// Arena version - no need to free manually
XR_FUNC XrHashMap* xr_hashmap_new_in_arena(struct XrArena *arena);

// Only for malloc version, not arena version
XR_FUNC void xr_hashmap_free(XrHashMap *map);

XR_FUNC void xr_hashmap_set(XrHashMap *map, const char *key, void *value);

// Returns NULL if key not found
XR_FUNC void* xr_hashmap_get(XrHashMap *map, const char *key);

XR_FUNC bool xr_hashmap_has(XrHashMap *map, const char *key);
XR_FUNC bool xr_hashmap_delete(XrHashMap *map, const char *key);
XR_FUNC void xr_hashmap_clear(XrHashMap *map);

// Iteration support (avoids exposing internal structure)
typedef void (*XrHashMapIterFunc)(const char *key, void *value, void *userdata);
XR_FUNC void xr_hashmap_foreach(XrHashMap *map, XrHashMapIterFunc func, void *userdata);

// Get count of active entries
static inline uint32_t xr_hashmap_count(XrHashMap *map) {
    return map ? map->count : 0;
}

#define XR_HASHMAP_MIN_CAPACITY 8
// Load factor 75%: resize when count * 4 >= capacity * 3
#define XR_HASHMAP_GROW_FACTOR 2

#endif // XHASHMAP_H
