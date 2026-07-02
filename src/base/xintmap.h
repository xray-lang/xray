/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xintmap.h - Integer-keyed hash table for compiler use
 *
 * KEY CONCEPT:
 *   Simple uint32_t->void* hash table using open addressing + linear probing.
 *   Designed for compiler internal use (symbol ID lookups, etc.).
 *
 * VS xhashmap.h:
 *   - xhashmap.h: String keys (variable names -> values)
 *   - xintmap.h: Integer keys (symbol IDs -> metadata)
 *
 * USE CASES:
 *   - Analyzer symbol links (symbol ID -> XaSymbolLinks*)
 *   - Type cache (type ID -> cached type)
 *   - Any uint32_t -> void* mapping in compiler
 */

#ifndef XINTMAP_H
#define XINTMAP_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "xdefs.h"

// Special marker values for empty/tombstone slots
#define XR_INTMAP_EMPTY 0xFFFFFFFF
#define XR_INTMAP_TOMBSTONE 0xFFFFFFFE

typedef struct XrIntMapEntry {
    uint32_t key;
    void *value;
} XrIntMapEntry;

typedef struct XrIntMap {
    XrIntMapEntry *entries;
    uint32_t capacity;
    uint32_t count;       // Active entries (excluding tombstones)
    uint32_t tombstones;  // Tombstone count for rehash decision
} XrIntMap;

// Create integer map (uses malloc)
XR_FUNC XrIntMap *xr_intmap_new(void);

XR_FUNC void xr_intmap_free(XrIntMap *map);

// Insert or update. Returns true on success; false when the key is a
// reserved sentinel (XR_INTMAP_EMPTY/TOMBSTONE) or when the key is
// absent and no slot is available (allocation failure on a full table).
// The map never silently drops an insert — callers must handle false.
XR_FUNC bool xr_intmap_set(XrIntMap *map, uint32_t key, void *value);

// Returns NULL if key not found
XR_FUNC void *xr_intmap_get(XrIntMap *map, uint32_t key);

// Check if key exists
XR_FUNC bool xr_intmap_has(XrIntMap *map, uint32_t key);

// Delete key (returns true if found and deleted)
XR_FUNC bool xr_intmap_delete(XrIntMap *map, uint32_t key);

// Clear all entries
XR_FUNC void xr_intmap_clear(XrIntMap *map);

// Iteration support
typedef void (*XrIntMapIterFunc)(uint32_t key, void *value, void *userdata);
XR_FUNC void xr_intmap_foreach(XrIntMap *map, XrIntMapIterFunc func, void *userdata);

#define XR_INTMAP_MIN_CAPACITY 16
#define XR_INTMAP_LOAD_FACTOR 0.75
#define XR_INTMAP_GROW_FACTOR 2

#endif  // XINTMAP_H
