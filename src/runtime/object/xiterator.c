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
#include "xmap.h" // Must be included before xarray.h, XrMap definition is required
#include "xset.h"
#include "xarray.h"
#include "xjson.h"
#include "../symbol/xsymbol_table.h"
#include "xstring.h"
#include "../../base/xmalloc.h"
#include "../gc/xgc_header.h"
#include "../gc/xgc.h"
#include <stdlib.h>
#include <string.h>

// Create iterator from Map (lazy, no pre-generation)
XrIterator* xr_iterator_new_from_map(struct XrCoroutine *coro, struct XrMap *map_param) {
    XR_DCHECK(coro != NULL, "iterator_new_from_map: NULL coro");
    XR_DCHECK(map_param != NULL, "iterator_new_from_map: NULL map");
    // Allocate Iterator on coroutine heap
    XrIterator *iter = (XrIterator*)xr_alloc(coro, sizeof(XrIterator), XR_TITERATOR);
    
    if (!iter) {
        return NULL;
    }

    // Initialize GC header
    xr_gc_header_init_type(&iter->gc, XR_TITERATOR);

    // Lazy mode: reference Map directly
    iter->source.map = (XrMap*)map_param;
    iter->scan_index = 0;
    iter->coro = coro;

    iter->type = XR_ITERATOR_MAP;
    return iter;
}

// Create iterator from Set (lazy, no pre-generation)
XrIterator* xr_iterator_new_from_set(struct XrCoroutine *coro, struct XrSet *set_param) {
    XR_DCHECK(coro != NULL, "iterator_new_from_set: NULL coro");
    XR_DCHECK(set_param != NULL, "iterator_new_from_set: NULL set");
    // Allocate Iterator on coroutine heap
    XrIterator *iter = (XrIterator*)xr_alloc(coro, sizeof(XrIterator), XR_TITERATOR);
    
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

    return iter;
}

// Create iterator from Json (lazy, converts SymbolId keys to strings)
XrIterator* xr_iterator_new_from_json(struct XrCoroutine *coro, struct XrJson *json,
                                       struct XrayIsolate *isolate) {
    XR_DCHECK(coro != NULL, "iterator_new_from_json: NULL coro");
    XR_DCHECK(json != NULL, "iterator_new_from_json: NULL json");
    XrIterator *iter = (XrIterator*)xr_alloc(coro, sizeof(XrIterator), XR_TITERATOR);
    if (!iter) return NULL;
    
    xr_gc_header_init_type(&iter->gc, XR_TITERATOR);
    iter->type = XR_ITERATOR_JSON;
    iter->source.json = json;
    iter->scan_index = 0;
    iter->coro = coro;
    iter->context = (void*)isolate;
    
    XrShape *shape = xr_json_shape(json);
    iter->total_count = shape ? shape->field_count : 0;
    
    return iter;
}

// Check if more elements available (advances scan_index past empty slots)
bool xr_iterator_has_next(XrIterator *iter) {
    if (!iter) {
        return false;
    }

    if (iter->type == XR_ITERATOR_MAP) {
        if (!iter->source.map) return false;

        XrMap *map = (XrMap*)iter->source.map;
        if (xr_map_isdummy(map)) return false;
        
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
        if (!iter->source.set) return false;

        XrSet *set = (XrSet*)iter->source.set;
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
        if (!json) return false;
        
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

        XrMap *map = (XrMap*)iter->source.map;
        if (xr_map_isdummy(map)) return xr_null();
        
        uint32_t size = xr_map_sizenode(map);
        while (iter->scan_index < size) {
            XrMapNode *node = xr_map_node(map, iter->scan_index);
            iter->scan_index++;  // Move to next position

            // Skip empty nodes
            if (!XR_MAP_NODE_EMPTY(node)) {
                // Found valid node, create [k,v] array on-demand
                XrArray *pair = xr_array_with_capacity(iter->coro, 2);
                if (!pair) {
                    return xr_null();
                }

                // Add elements manually
                ((XrValue*)pair->data)[0] = node->key;
                ((XrValue*)pair->data)[1] = node->value;
                pair->length = 2;

                return xr_value_from_array(pair);
            }
        }

        // Scan complete, no more elements
        return xr_null();
    } else if (iter->type == XR_ITERATOR_SET) {
        // Set iterator: returns single value
        if (!iter->source.set) {
            return xr_null();
        }

        XrSet *set = (XrSet*)iter->source.set;
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
        if (!json) return xr_null();
        XrayIsolate *X = (XrayIsolate*)iter->context;
        XrSymbolTable *st = X ? (XrSymbolTable*)xr_isolate_get_symbol_table(X) : NULL;
        
        {
            XrShape *shape = xr_json_shape(json);
            if (!shape || iter->scan_index >= shape->field_count) return xr_null();
            
            uint32_t idx = iter->scan_index++;
            SymbolId sym = shape->field_symbols[idx];
            XrValue value = xr_json_get_field_any(json, idx);
            
            XrArray *pair = xr_array_with_capacity(iter->coro, 2);
            if (!pair) return xr_null();
            
            // Convert SymbolId to string
            XrValue key_str = xr_null();
            if (st) {
                const char *name = xr_symbol_get_name_in_table(st, sym);
                if (name) {
                    key_str = xr_string_value(xr_string_intern(X, name, strlen(name), 0));
                }
            }
            ((XrValue*)pair->data)[0] = key_str;
            ((XrValue*)pair->data)[1] = value;
            pair->length = 2;
            return xr_value_from_array(pair);
        }
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
XrIterator* xr_value_to_iterator(XrValue value) {
    if (!XR_IS_ITERATOR(value)) {
        return NULL;
    }

    return (XrIterator*)XR_TO_PTR(value);
}

