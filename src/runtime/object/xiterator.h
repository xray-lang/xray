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

/* ========== Iterator Type Constants ========== */

typedef enum {
    XR_ITERATOR_MAP = 0,
    XR_ITERATOR_SET = 1,
    XR_ITERATOR_JSON = 2
} XrIteratorType;

/* ========== Iterator Structure ========== */

/*
 * XrIterator - Lazy iterator for Map/Set
 *
 * Generates [key, value] pairs on-demand instead of pre-building entries array.
 * Supports both Map and Set iteration.
 *
 * Advantages:
 * - Memory: only Iterator object (16-24 bytes) + single temp [k,v] array (24 bytes)
 * - Performance: avoids pre-generating all entries array
 * - Early exit: if loop breaks, unvisited elements are never created
 */
typedef struct XrIterator {
    XrGCHeader gc;        // GC header
    XrIteratorType type;  // iterator type (Map or Set)
    union {
        struct XrMap *map;     // Map iterator: reference to source Map
        struct XrSet *set;     // Set iterator: reference to source Set
        struct XrJson *json;   // Json iterator: reference to source Json
    } source;                  // source object (union, selected by type)
    uint32_t scan_index;       // internal scan position
    uint32_t total_count;      // total field count (Json fast mode only)
    struct XrCoroutine *coro;  // coroutine (for creating temp arrays)
    void *context;             // extra context (Json: XrSymbolTable*)
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

#endif  // XITERATOR_H
