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
 * KEY LIFETIME CONTRACT:
 *   Keys are NOT copied. The map stores the caller's pointer, so every
 *   key must outlive the map (interned strings, arena-allocated names,
 *   or string literals). Values are opaque pointers, never owned.
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
#include "xio_policy.h"

// Tombstone sentinel: key==NULL && value==XR_HASHMAP_TOMBSTONE
#define XR_HASHMAP_TOMBSTONE ((void *) (uintptr_t) 1)

typedef struct XrHashMapEntry {
    char *key;
    void *value;
    uint32_t hash;  // Cached hash (avoids recompute on probe/rehash)
} XrHashMapEntry;

typedef struct XrHashMap {
    XrHashMapEntry *entries;
    uint32_t capacity;
    uint32_t count;
} XrHashMap;

/* The mandatory policy is copied into private aligned owner storage. Keys and
 * values remain borrowed. Failed mutations preserve all prior entries; work
 * and cumulative allocations stay consumed. Owned blocks keep a compile ledger
 * alive, while other policy contexts must outlive this map. */
XR_FUNC XrOsIoStatus xr_hashmap_owned_new(const XrOsIoPolicy *policy, XrHashMap **output);
XR_FUNC void xr_hashmap_owned_free(XrHashMap *map);
XR_FUNC XrOsIoStatus xr_hashmap_owned_set(XrHashMap *map, const char *key, void *value);
/* Missing keys publish NULL on OK. Resource failures preserve output. */
XR_FUNC XrOsIoStatus xr_hashmap_owned_get(const XrHashMap *map, const char *key, void **output);
XR_FUNC XrOsIoStatus xr_hashmap_owned_has(const XrHashMap *map, const char *key, bool *output);
XR_FUNC XrOsIoStatus xr_hashmap_owned_delete(XrHashMap *map, const char *key, bool *output);
XR_FUNC XrOsIoStatus xr_hashmap_owned_clear(XrHashMap *map);

typedef void (*XrHashMapIterFunc)(const char *key, void *value, void *userdata);
/* Iteration callbacks cannot mutate the map. */
XR_FUNC XrOsIoStatus xr_hashmap_owned_foreach(const XrHashMap *map, XrHashMapIterFunc func, void *userdata);
/* Cleanup invokes each callback once without fresh work admission, then frees
 * the map. Callbacks may release borrowed keys/values but cannot fail or mutate
 * the map. A NULL callback is equivalent to owned_free. */
XR_FUNC void xr_hashmap_owned_dispose(XrHashMap *map, XrHashMapIterFunc cleanup, void *userdata);
// Get count of active entries
static inline uint32_t xr_hashmap_count(XrHashMap *map) {
    return map ? map->count : 0;
}

#define XR_HASHMAP_MIN_CAPACITY 8
// Load factor 75%: resize when count * 4 >= capacity * 3
#define XR_HASHMAP_GROW_FACTOR 2

#endif  // XHASHMAP_H
