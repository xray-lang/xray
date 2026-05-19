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
#include "../gc/xalloc_unified.h"
#include "../class/xclass.h"
#include "../class/xclass_builder.h"
#include "../class/xclass_system.h"
#include "../class/xinstance.h"
#include <stdlib.h>
#include <string.h>

/* Internal helper: allocate iterator instance with klass set. */
static XrIterator *iterator_alloc(struct XrCoroutine *coro) {
    XrayIsolate *X = xr_coro_get_isolate(coro);
    XrClass *cls = xr_isolate_get_core_classes(X)->iteratorClass;
    XR_DCHECK(cls != NULL, "iterator_alloc: iteratorClass not registered");
    XrIterator *iter = (XrIterator *) xr_alloc(coro, sizeof(XrIterator), XR_TINSTANCE);
    if (!iter)
        return NULL;
    iter->klass = cls;
    return iter;
}

// Create iterator from Map (lazy, no pre-generation). Default mode = PAIRS:
// next() yields (k, v) tuples. Used by entriesIterator and `for (k, v in m)`.
XrIterator *xr_iterator_new_from_map(struct XrCoroutine *coro, struct XrMap *map_param) {
    XR_DCHECK(coro != NULL, "iterator_new_from_map: NULL coro");
    XR_DCHECK(map_param != NULL, "iterator_new_from_map: NULL map");
    XrIterator *iter = iterator_alloc(coro);
    if (!iter)
        return NULL;

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
    XrIterator *iter = iterator_alloc(coro);
    if (!iter)
        return NULL;

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
    XrIterator *iter = iterator_alloc(coro);
    if (!iter)
        return NULL;
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
    XrIterator *iter = iterator_alloc(coro);
    if (!iter)
        return NULL;
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
XrIterator *xr_iterator_new_from_json(struct XrCoroutine *coro, XrJson *json,
                                      struct XrayIsolate *isolate) {
    XR_DCHECK(coro != NULL, "iterator_new_from_json: NULL coro");
    XR_DCHECK(json != NULL, "iterator_new_from_json: NULL json");
    XrIterator *iter = iterator_alloc(coro);
    if (!iter)
        return NULL;

    iter->type = XR_ITERATOR_JSON;
    iter->source.json = json;
    iter->scan_index = 0;
    iter->coro = coro;
    iter->context = (void *) isolate;

    iter->total_count = json && json->klass ? json->klass->field_count : 0;
    iter->mode = XR_ITER_MODE_PAIRS;

    return iter;
}

// Same source as xr_iterator_new_from_json, but next() yields each key
// (a string) instead of a (key, value) tuple. Used by single-variable
// `for (k in jsonObj)`.
XrIterator *xr_iterator_keys_from_json(struct XrCoroutine *coro, XrJson *json,
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
        if (!json || !json->klass)
            return xr_null();
        XrayIsolate *X = (XrayIsolate *) iter->context;

        {
            XrClass *cls = json->klass;
            if (iter->scan_index >= cls->field_count)
                return xr_null();

            uint32_t idx = iter->scan_index++;
            const char *name = cls->fields[idx].name;
            XrValue value = xr_instance_get_dynamic_field(json, (uint16_t) idx);

            // Field name comes from the class descriptor directly
            XrValue key_str = xr_null();
            if (name) {
                key_str = xr_string_value(xr_string_intern(X, name, strlen(name), 0));
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
        if (iter->mode == XR_ITER_MODE_VALUES) {
            return elem;
        }
        if (iter->mode == XR_ITER_MODE_KEYS) {
            return xr_int((int64_t) idx);
        }
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
        if (iter->mode == XR_ITER_MODE_VALUES) {
            return ch ? xr_string_value(ch) : xr_null();
        }
        if (iter->mode == XR_ITER_MODE_KEYS) {
            return xr_int((int64_t) idx);
        }
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

/* ========== Native Body Lifecycle ========== */

#include "../gc/xcoro_gc.h"

/* Native body traverse: marks the source container so it survives GC
 * for the lifetime of the iterator. coro / context are non-GC. */
static void iterator_body_traverse(struct XrCoroGC *gc, void *body) {
    /* body points to the iterator's `type` field; we recover the enclosing
     * XrIterator by subtracting the offset that xinstance.c uses. The body
     * layout starts right after `klass`, so casting back is safe. */
    XrIterator *iter = (XrIterator *) ((char *) body - offsetof(XrIterator, type));
    switch (iter->type) {
        case XR_ITERATOR_MAP:
            if (iter->source.map)
                xr_coro_gc_markobject(gc, (XrGCHeader *) iter->source.map);
            break;
        case XR_ITERATOR_SET:
            if (iter->source.set)
                xr_coro_gc_markobject(gc, (XrGCHeader *) iter->source.set);
            break;
        case XR_ITERATOR_JSON:
            if (iter->source.json)
                xr_coro_gc_markobject(gc, (XrGCHeader *) iter->source.json);
            break;
        case XR_ITERATOR_ARRAY:
            if (iter->source.array)
                xr_coro_gc_markobject(gc, (XrGCHeader *) iter->source.array);
            break;
        case XR_ITERATOR_STRING:
            if (iter->source.string)
                xr_coro_gc_markobject(gc, (XrGCHeader *) iter->source.string);
            break;
    }
}

static XrNativeBodyDesc g_iterator_body_desc = {
    .body_size = sizeof(XrIterator) - offsetof(XrIterator, type),
    .body_align = _Alignof(void *),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = NULL,
    .destroy = NULL,
    .traverse = iterator_body_traverse,
    .deep_copy = NULL,
    .to_shared = NULL,
};

XrNativeBodyDesc *xr_iterator_native_body_desc(void) {
    return &g_iterator_body_desc;
}

/* ========== Class Registration ========== */

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

void xr_iterator_register_class(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "iterator_register_class: NULL isolate");
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XR_DCHECK(core != NULL, "iterator_register_class: core not initialised");
    XR_DCHECK(core->iteratorClass == NULL, "iterator_register_class: already registered");

    XrClassBuilder *b = xr_class_builder_new(X, "Iterator", core->objectClass);
    XR_CHECK(b != NULL, "iterator_register_class: builder alloc failed");

    xr_class_builder_set_native_body(b, xr_iterator_native_body_desc());
    xr_class_builder_add_method(b, "hasNext", m_iter_has_next, 0, 0);
    xr_class_builder_add_method(b, "next", m_iter_next, 0, 0);
    xr_class_builder_add_method(b, "toString", m_iter_to_string, 0, 0);

    XrClass *cls = xr_class_builder_finalize(b);
    XR_CHECK(cls != NULL, "iterator_register_class: finalize failed");
    cls->flags |= XR_CLASS_BUILTIN | XR_CLASS_HAS_NATIVE_BODY | XR_CLASS_ITERATOR;
    core->iteratorClass = cls;
}
