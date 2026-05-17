/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xiterator.h - Iterator for for-in loops
 *
 * KEY CONCEPT:
 *   Supports Map/Set iteration with key-value pairs.
 */

#ifndef XITERATOR_H
#define XITERATOR_H

#include "../value/xvalue.h"
#include "../gc/xgc_header.h"
#include "xarray.h"
#include <stdint.h>
#include <stdbool.h>

struct XrMap;
struct XrSet;
struct XrJson;
struct XrArray;
struct XrString;

/* ========== Iterator Type Constants ========== */

typedef enum {
    XR_ITERATOR_MAP = 0,
    XR_ITERATOR_SET = 1,
    XR_ITERATOR_JSON = 2,
    XR_ITERATOR_ARRAY = 3,
    XR_ITERATOR_STRING = 4,
} XrIteratorType;

/* ========== Iterator Structure ========== */

/*
 * XrIterator - Lazy iterator for Map/Set/Json/Array/String
 *
 * Generates [key, value] (or [index, element]) pairs on-demand instead of
 * pre-building an entries array. Used by `for (k, v in coll)` lowering: the
 * compiler always invokes `coll.entriesIterator()` and pulls pairs one by
 * one with hasNext()/next().
 *
 * Advantages:
 * - Memory: only Iterator object (~32 bytes) + single temp [k,v] array (24 bytes)
 * - Performance: avoids pre-generating all entries array
 * - Early exit: if loop breaks, unvisited elements are never created
 */
typedef struct XrIterator {
    XrGCHeader gc;        // GC header
    XrIteratorType type;  // iterator kind
    union {
        struct XrMap *map;        // Map iterator
        struct XrSet *set;        // Set iterator
        struct XrJson *json;      // Json iterator
        struct XrArray *array;    // Array iterator
        struct XrString *string;  // String iterator
    } source;                     // source object (union, selected by type)
    uint32_t scan_index;          // internal scan position
    uint32_t total_count;         // total field count (Json fast mode only)
    struct XrCoroutine *coro;     // coroutine (for creating temp arrays)
    void *context;                // extra context (Json: XrSymbolTable*)
} XrIterator;

/* ========== Iterator API ========== */

// Create iterator from Map (lazy, no pre-generation)
struct XrCoroutine;
XR_FUNC XrIterator *xr_iterator_new_from_map(struct XrCoroutine *coro, struct XrMap *map);

// Create iterator from Set (lazy, no pre-generation)
XR_FUNC XrIterator *xr_iterator_new_from_set(struct XrCoroutine *coro, struct XrSet *set);

// Create iterator from Json (lazy, converts SymbolId keys to strings)
XR_FUNC XrIterator *xr_iterator_new_from_json(struct XrCoroutine *coro, struct XrJson *json,
                                              struct XrayIsolate *isolate);

// Create iterator from Array (lazy, yields [index, element] pairs)
XR_FUNC XrIterator *xr_iterator_new_from_array(struct XrCoroutine *coro, struct XrArray *arr);

// Create iterator from string (lazy, yields [index, char] pairs).
// `isolate` is needed so we can intern the per-rune single-character strings.
XR_FUNC XrIterator *xr_iterator_new_from_string(struct XrCoroutine *coro, struct XrString *s,
                                                struct XrayIsolate *isolate);

// Check if more elements available
XR_FUNC bool xr_iterator_has_next(XrIterator *iter);

// Get next element ([key, value] array, null if exhausted)
XR_FUNC XrValue xr_iterator_next(XrIterator *iter);

/* ========== XrValue Operations ========== */

XR_FUNC XrValue xr_value_from_iterator(XrIterator *iter);
XR_FUNC XrIterator *xr_value_to_iterator(XrValue value);

static inline bool xr_is_iterator(XrValue value) {
    return XR_IS_ITERATOR(value);
}

/* Register Iterator as a native type for unified dispatch. */
struct XrayIsolate;
XR_FUNC void xr_iterator_register_native_type(struct XrayIsolate *isolate);

#endif  // XITERATOR_H
