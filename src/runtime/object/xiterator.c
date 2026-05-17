/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xiterator.c - Iterator implementation
 *
 * KEY CONCEPT:
 *   Lazy iterator for Map/Set, generates [key, value] pairs on-demand.
 */

#include "xiterator.h"
#include "../xisolate_api.h"
#include "../../base/xchecks.h"
#include "xmap.h"  // Must be included before xarray.h, XrMap definition is required
#include "xset.h"
#include "xarray.h"
#include "xtuple.h"
#include "xjson.h"
#include "../symbol/xsymbol_table.h"
#include "xstring.h"
#include "../../base/xmalloc.h"
#include "../gc/xgc_header.h"
#include "../gc/xgc.h"
#include <stdlib.h>
#include <string.h>

// Create iterator from Map (lazy, no pre-generation). Default mode = PAIRS:
// next() yields (k, v) tuples. Used by entriesIterator and `for (k, v in m)`.
XrIterator *xr_iterator_new_from_map(struct XrCoroutine *coro, struct XrMap *map_param) {
    XR_DCHECK(coro != NULL, "iterator_new_from_map: NULL coro");
    XR_DCHECK(map_param != NULL, "iterator_new_from_map: NULL map");
    // Allocate Iterator on coroutine heap
    XrIterator *iter = (XrIterator *) xr_alloc(coro, sizeof(XrIterator), XR_TITERATOR);

    if (!iter) {
        return NULL;
    }

    // Initialize GC header
    xr_gc_header_init_type(&iter->gc, XR_TITERATOR);

    // Lazy mode: reference Map directly
    iter->source.map = (XrMap *) map_param;
    iter->scan_index = 0;
    iter->coro = coro;

    iter->type = XR_ITERATOR_MAP;
    iter->mode = XR_ITER_MODE_PAIRS;
    return iter;
}

// Same source as xr_iterator_new_from_map, but next() yields each key K
// instead of a (K, V) tuple. Used by single-variable `for (k in m)`.
XrIterator *xr_iterator_keys_from_map(struct XrCoroutine *coro, struct XrMap *map_param) {
    XrIterator *iter = xr_iterator_new_from_map(coro, map_param);
    if (iter)
        iter->mode = XR_ITER_MODE_KEYS;
    return iter;
}

// Create iterator from Set (lazy, no pre-generation)
XrIterator *xr_iterator_new_from_set(struct XrCoroutine *coro, struct XrSet *set_param) {
    XR_DCHECK(coro != NULL, "iterator_new_from_set: NULL coro");
    XR_DCHECK(set_param != NULL, "iterator_new_from_set: NULL set");
    // Allocate Iterator on coroutine heap
    XrIterator *iter = (XrIterator *) xr_alloc(coro, sizeof(XrIterator), XR_TITERATOR);

    if (!iter) {
        return NULL;
    }

    // Initialize GC header
    xr_gc_header_init_type(&iter->gc, XR_TITERATOR);

    // Lazy mode: reference Set directly
    iter->type = XR_ITERATOR_SET;
    iter->source.set = set_param;
    iter->scan_index = 0;
    iter->coro = coro;
    iter->mode = XR_ITER_MODE_VALUES;  // Set has no separate key projection.

    return iter;
}

// Create iterator from Array (lazy, yields [index, element] pairs)
XrIterator *xr_iterator_new_from_array(struct XrCoroutine *coro, struct XrArray *arr) {
    XR_DCHECK(coro != NULL, "iterator_new_from_array: NULL coro");
    XR_DCHECK(arr != NULL, "iterator_new_from_array: NULL array");
    XrIterator *iter = (XrIterator *) xr_alloc(coro, sizeof(XrIterator), XR_TITERATOR);
    if (!iter)
        return NULL;
    xr_gc_header_init_type(&iter->gc, XR_TITERATOR);
    iter->type = XR_ITERATOR_ARRAY;
    iter->source.array = arr;
    iter->scan_index = 0;
    iter->coro = coro;
    iter->total_count = 0;
    iter->context = NULL;
    iter->mode = XR_ITER_MODE_PAIRS;
    return iter;
}

// Create iterator from string (lazy, yields [index, char] pairs).
// The index is the UTF-8 character index, matching string.charAt() semantics.
XrIterator *xr_iterator_new_from_string(struct XrCoroutine *coro, struct XrString *s,
                                        struct XrayIsolate *isolate) {
    XR_DCHECK(coro != NULL, "iterator_new_from_string: NULL coro");
    XR_DCHECK(s != NULL, "iterator_new_from_string: NULL string");
    XrIterator *iter = (XrIterator *) xr_alloc(coro, sizeof(XrIterator), XR_TITERATOR);
    if (!iter)
        return NULL;
    xr_gc_header_init_type(&iter->gc, XR_TITERATOR);
    iter->type = XR_ITERATOR_STRING;
    iter->source.string = s;
    iter->scan_index = 0;
    iter->coro = coro;
    iter->total_count = (uint32_t) xr_string_char_length(s);
    iter->context = (void *) isolate;
    iter->mode = XR_ITER_MODE_PAIRS;
    return iter;
}

// Create iterator from Json (lazy, converts SymbolId keys to strings)
XrIterator *xr_iterator_new_from_json(struct XrCoroutine *coro, struct XrJson *json,
                                      struct XrayIsolate *isolate) {
    XR_DCHECK(coro != NULL, "iterator_new_from_json: NULL coro");
    XR_DCHECK(json != NULL, "iterator_new_from_json: NULL json");
    XrIterator *iter = (XrIterator *) xr_alloc(coro, sizeof(XrIterator), XR_TITERATOR);
    if (!iter)
        return NULL;

    xr_gc_header_init_type(&iter->gc, XR_TITERATOR);
    iter->type = XR_ITERATOR_JSON;
    iter->source.json = json;
    iter->scan_index = 0;
    iter->coro = coro;
    iter->context = (void *) isolate;

    XrShape *shape = xr_json_shape(isolate, json);
    iter->total_count = shape ? shape->field_count : 0;
    iter->mode = XR_ITER_MODE_PAIRS;

    return iter;
}

// Same source as xr_iterator_new_from_json, but next() yields each key
// (a string) instead of a (key, value) tuple. Used by single-variable
// `for (k in jsonObj)`.
XrIterator *xr_iterator_keys_from_json(struct XrCoroutine *coro, struct XrJson *json,
                                       struct XrayIsolate *isolate) {
    XrIterator *iter = xr_iterator_new_from_json(coro, json, isolate);
    if (iter)
        iter->mode = XR_ITER_MODE_KEYS;
    return iter;
}

// Check if more elements available (advances scan_index past empty slots)
bool xr_iterator_has_next(XrIterator *iter) {
    if (!iter) {
        return false;
    }

    if (iter->type == XR_ITERATOR_MAP) {
        if (!iter->source.map)
            return false;

        XrMap *map = (XrMap *) iter->source.map;
        if (xr_map_isdummy(map))
            return false;

        uint32_t size = xr_map_sizenode(map);
        // Skip empty nodes, park scan_index at next valid node
        while (iter->scan_index < size) {
            XrMapNode *node = xr_map_node(map, iter->scan_index);
            if (!XR_MAP_NODE_EMPTY(node)) {
                return true;
            }
            iter->scan_index++;
        }
        return false;
    } else if (iter->type == XR_ITERATOR_SET) {
        if (!iter->source.set)
            return false;

        XrSet *set = (XrSet *) iter->source.set;
        // Skip empty/tombstone entries, park scan_index at next valid entry
        while (iter->scan_index < set->capacity) {
            if (set->entries[iter->scan_index].state >= 0x80) {
                return true;
            }
            iter->scan_index++;
        }
        return false;
    } else if (iter->type == XR_ITERATOR_JSON) {
        XrJson *json = iter->source.json;
        if (!json)
            return false;

        return iter->scan_index < iter->total_count;
    } else if (iter->type == XR_ITERATOR_ARRAY) {
        XrArray *arr = iter->source.array;
        if (!arr)
            return false;
        return (int32_t) iter->scan_index < arr->length;
    } else if (iter->type == XR_ITERATOR_STRING) {
        if (!iter->source.string)
            return false;
        return iter->scan_index < iter->total_count;
    }

    return false;
}

// Get next element (lazy, creates [k,v] array on-demand)
XrValue xr_iterator_next(XrIterator *iter) {
    if (!iter) {
        return xr_null();
    }

    if (iter->type == XR_ITERATOR_MAP) {
        // Map iterator (chained hash): returns [key, value] array
        if (!iter->source.map) {
            return xr_null();
        }

        XrMap *map = (XrMap *) iter->source.map;
        if (xr_map_isdummy(map))
            return xr_null();

        uint32_t size = xr_map_sizenode(map);
        while (iter->scan_index < size) {
            XrMapNode *node = xr_map_node(map, iter->scan_index);
            iter->scan_index++;  // Move to next position

            // Skip empty nodes
            if (!XR_MAP_NODE_EMPTY(node)) {
                if (iter->mode == XR_ITER_MODE_KEYS) {
                    return node->key;
                }
                if (iter->mode == XR_ITER_MODE_VALUES) {
                    return node->value;
                }
                /* PAIRS: build a (key, value) tuple on demand. Used by
                 * entriesIterator() and `for (k, v in m)`. */
                XrTuple *pair = xr_tuple_new(iter->coro, 2);
                if (!pair)
                    return xr_null();
                xr_tuple_set(pair, 0, node->key);
                xr_tuple_set(pair, 1, node->value);
                return xr_value_from_tuple(pair);
            }
        }

        // Scan complete, no more elements
        return xr_null();
    } else if (iter->type == XR_ITERATOR_SET) {
        // Set iterator: returns single value
        if (!iter->source.set) {
            return xr_null();
        }

        XrSet *set = (XrSet *) iter->source.set;
        while (iter->scan_index < set->capacity) {
            XrSetEntry *entry = &set->entries[iter->scan_index];
            iter->scan_index++;  // Move to next position

            // Skip empty slots and tombstones
            if (entry->state >= 0x80) {
                // Found valid entry, return value directly
                return entry->value;
            }
        }

        // Scan complete, no more elements
        return xr_null();
    } else if (iter->type == XR_ITERATOR_JSON) {
        // Json iterator: returns [string_key, value] array
        XrJson *json = iter->source.json;
        if (!json)
            return xr_null();
        XrayIsolate *X = (XrayIsolate *) iter->context;
        XrSymbolTable *st = X ? (XrSymbolTable *) xr_isolate_get_symbol_table(X) : NULL;

        {
            XrShape *shape = xr_json_shape(X, json);
            if (!shape || iter->scan_index >= shape->field_count)
                return xr_null();

            uint32_t idx = iter->scan_index++;
            SymbolId sym = shape->field_symbols[idx];
            XrValue value = xr_json_get_field_any(X, json, idx);

            // Convert SymbolId to string
            XrValue key_str = xr_null();
            if (st) {
                const char *name = xr_symbol_get_name_in_table(st, sym);
                if (name) {
                    key_str = xr_string_value(xr_string_intern(X, name, strlen(name), 0));
                }
            }
            if (iter->mode == XR_ITER_MODE_KEYS) {
                return key_str;
            }
            if (iter->mode == XR_ITER_MODE_VALUES) {
                return value;
            }
            XrTuple *pair = xr_tuple_new(iter->coro, 2);
            if (!pair)
                return xr_null();
            xr_tuple_set(pair, 0, key_str);
            xr_tuple_set(pair, 1, value);
            return xr_value_from_tuple(pair);
        }
    } else if (iter->type == XR_ITERATOR_ARRAY) {
        XrArray *arr = iter->source.array;
        if (!arr || (int32_t) iter->scan_index >= arr->length)
            return xr_null();
        uint32_t idx = iter->scan_index++;
        XrValue elem = xr_array_get_element(arr, (int32_t) idx);
        XrTuple *pair = xr_tuple_new(iter->coro, 2);
        if (!pair)
            return xr_null();
        xr_tuple_set(pair, 0, xr_int((int64_t) idx));
        xr_tuple_set(pair, 1, elem);
        return xr_value_from_tuple(pair);
    } else if (iter->type == XR_ITERATOR_STRING) {
        XrString *s = iter->source.string;
        if (!s || iter->scan_index >= iter->total_count)
            return xr_null();
        XrayIsolate *X = (XrayIsolate *) iter->context;
        uint32_t idx = iter->scan_index++;
        XrString *ch = xr_string_char_at_unicode(X, s, (size_t) idx);
        XrTuple *pair = xr_tuple_new(iter->coro, 2);
        if (!pair)
            return xr_null();
        xr_tuple_set(pair, 0, xr_int((int64_t) idx));
        xr_tuple_set(pair, 1, ch ? xr_string_value(ch) : xr_null());
        return xr_value_from_tuple(pair);
    }

    return xr_null();
}

// Convert Iterator to XrValue
XrValue xr_value_from_iterator(XrIterator *iter) {
    if (!iter) {
        return xr_null();
    }
    return XR_FROM_PTR(iter);
}

// Extract Iterator from XrValue
XrIterator *xr_value_to_iterator(XrValue value) {
    if (!XR_IS_ITERATOR(value)) {
        return NULL;
    }

    return (XrIterator *) XR_TO_PTR(value);
}

/* ========== Native Type Registration ========== */

#include "xnative_type.h"
#include "xstring.h"
#include "../value/xvalue_format.h"

static XrValue m_iter_has_next(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    XrIterator *iter = xr_value_to_iterator(self);
    XR_DCHECK(iter != NULL, "Iterator.hasNext: invalid iterator");
    return xr_bool(xr_iterator_has_next(iter));
}

static XrValue m_iter_next(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    XrIterator *iter = xr_value_to_iterator(self);
    XR_DCHECK(iter != NULL, "Iterator.next: invalid iterator");
    return xr_iterator_next(iter);
}

static XrValue m_iter_to_string(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    return xr_string_value(xr_value_to_string(iso, self));
}

void xr_iterator_register_native_type(XrayIsolate *isolate) {
    static const XrNativeMethod iter_methods[] = {
        {"hasNext", m_iter_has_next, 0},
        {"next", m_iter_next, 0},
        {"toString", m_iter_to_string, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo iter_info = {
        .name = "Iterator",
        .gc_type = XR_TITERATOR,
        .methods = iter_methods,
        .getters = NULL,
        .static_methods = NULL,
    };
    xr_register_native_type(isolate, &iter_info);
}
