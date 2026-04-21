/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson.c - Structured data object implementation
 *
 * KEY CONCEPT:
 *   - Properties stored inline at object tail
 *   - Shape pointer swap on new field (zero-copy)
 *   - No dictionary mode: Json is a pure data carrier
 */

#include "xjson.h"
#include "../../base/xchecks.h"
#include "../gc/xgc.h"
#include "../gc/xalloc_unified.h"
#include "../../base/xmalloc.h"
#include "../xisolate_api.h"
#include "../symbol/xsymbol_table.h"
#include <string.h>
#include <stdio.h>

// Get isolate's symbol table
static inline XrSymbolTable* get_symbol_table(XrayIsolate *isolate) {
    return (XrSymbolTable*)xr_isolate_get_symbol_table(isolate);
}

/* ========== Internal Functions ========== */

// Calculate Json object size
static inline size_t json_size(uint16_t capacity) {
    return sizeof(XrJson) + capacity * sizeof(XrValue);
}

/* ========== Root Shape Cache ========== */

// Root shapes cached per-isolate by capacity. Shape transitions branch from
// these roots, so identical field sequences converge to the same transition
// chain and avoid shape registry exhaustion.
#define ROOT_SHAPE_CACHE_SIZE 32

#include "../xisolate_internal.h"

static XrShape *get_or_create_root_shape(XrayIsolate *X, uint16_t capacity) {
    XR_DCHECK(X != NULL, "get_or_create_root_shape: NULL isolate");
    if (capacity < ROOT_SHAPE_CACHE_SIZE && X->root_shape_cache[capacity]) {
        return X->root_shape_cache[capacity];
    }
    XrShape *shape = xr_shape_new(X, capacity);
    if (shape && capacity < ROOT_SHAPE_CACHE_SIZE) {
        X->root_shape_cache[capacity] = shape;
    }
    return shape;
}

/* ========== Creation API Implementation ========== */

// Create empty Json object (Fast mode)
XrJson *xr_json_new(struct XrCoroutine *coro, uint16_t capacity) {
    XR_DCHECK(coro != NULL, "json_new: NULL coro");
    if (capacity == 0) {
        capacity = SHAPE_DEFAULT_CAPACITY;
    }

    // Get or create cached root shape for this capacity
    XrayIsolate *X = xr_coro_get_isolate(coro);
    XrShape *shape = get_or_create_root_shape(X, capacity);
    if (!shape) return NULL;

    // Allocate Json on coroutine heap
    size_t size = json_size(capacity);
    XrJson *json = (XrJson*)xr_alloc(coro, size, XR_TJSON);
    if (!json) return NULL;

    xr_json_set_shape(json, shape);
    json->overflow = NULL;

    // Initialize all fields to null
    // XR_NULL_VAL is all-zeros (tag=0, ptr=NULL, _pad=0), so memset is equivalent
    memset(json->fields, 0, capacity * sizeof(XrValue));

    return json;
}

// Create Json object with specified Shape
XrJson *xr_json_new_with_shape(struct XrCoroutine *coro, XrShape *shape) {
    XR_DCHECK(coro != NULL, "json_new_with_shape: NULL coro");
    if (!shape) return NULL;

    int field_count = shape->in_object_capacity;

    // Allocate Json on coroutine heap — lazy coro_gc creation
    size_t size = json_size(field_count);
    XrCoroGC *gc = xr_coro_ensure_gc(coro);
    if (!gc) return NULL;
    XrGCHeader *obj = xr_coro_gc_newobj(gc, XR_TJSON, size);
    if (!obj) return NULL;
    XrJson *json = (XrJson *)obj;

    xr_json_set_shape(json, shape);
    json->overflow = NULL;

    // Initialize all fields to null
    // XR_NULL_VAL is all-zeros (tag=0, ptr=NULL, _pad=0), so memset is equivalent
    memset(json->fields, 0, field_count * sizeof(XrValue));

    return json;
}

// Create Json object with specified Shape, skip field initialization.
// Caller MUST set all fields before any GC can run.
XrJson *xr_json_new_with_shape_noinit(struct XrCoroutine *coro, XrShape *shape) {
    if (!shape) return NULL;

    int field_count = shape->in_object_capacity;

    size_t size = json_size(field_count);
    XrCoroGC *gc2 = xr_coro_ensure_gc(coro);
    if (!gc2) return NULL;
    XrGCHeader *obj = xr_coro_gc_newobj(gc2, XR_TJSON, size);
    if (!obj) return NULL;
    XrJson *json = (XrJson *)obj;

    xr_json_set_shape(json, shape);
    json->overflow = NULL;
    return json;
}

// Initialize Json in-place on pre-allocated memory (for shared Json)
void xr_json_init_inplace(XrJson *json, XrShape *shape) {
    if (!json || !shape) return;

    xr_json_set_shape(json, shape);
    json->overflow = NULL;

    // Initialize all fields to null
    // XR_NULL_VAL is all-zeros, so memset is equivalent
    int field_count = shape->in_object_capacity;
    memset(json->fields, 0, field_count * sizeof(XrValue));
}

// Get Json object size (for system heap allocation)
size_t xr_json_size(int field_count) {
    return json_size(field_count);
}

/* ========== Field Access API Implementation ========== */

// Get field by Symbol — O(1) via Shape index lookup
XrValue xr_json_get(XrayIsolate *X, XrJson *json, SymbolId symbol) {
    if (!json) return xr_null();

    XrShape *shape = xr_json_shape(X, json);
    int idx = xr_shape_field_index(shape, symbol);
    if (idx < 0) return xr_null();

    // In-object fast path
    if (idx < shape->in_object_capacity) {
        XR_DCHECK(idx >= 0, "json_get: negative field index");
        return json->fields[idx];
    }
    // Overflow path
    XrJsonOverflow *ov = json->overflow;
    if (!ov) return xr_null();
    uint16_t ov_idx = (uint16_t)(idx - shape->in_object_capacity);
    if (ov_idx >= ov->length) return xr_null();
    XR_DCHECK(ov_idx < ov->capacity, "json_get: overflow index out of capacity");
    return ov->values[ov_idx];
}

// Allocate or grow overflow array. Initial capacity 8, doubles on grow.
static XrJsonOverflow *overflow_grow(XrJsonOverflow *old, uint16_t min_cap) {
    XR_DCHECK(min_cap > 0, "overflow_grow: zero min_cap");
    uint16_t new_cap = old ? old->capacity * 2 : 8;
    if (new_cap < min_cap) new_cap = min_cap;
    XrJsonOverflow *ov = (XrJsonOverflow *)xr_realloc(
        old, sizeof(XrJsonOverflow) + new_cap * sizeof(XrValue));
    if (!ov) return old;
    if (!old) {
        ov->length = 0;
    }
    // Zero-init new slots
    for (uint16_t i = ov->length; i < new_cap; i++) {
        ov->values[i] = xr_null();
    }
    ov->capacity = new_cap;
    return ov;
}

// Set field by Symbol
void xr_json_set(XrayIsolate *X, XrJson *json, SymbolId symbol, XrValue value) {
    XR_DCHECK(X != NULL, "json_set: NULL isolate");
    if (!json) return;

    XrShape *shape = xr_json_shape(X, json);

    // Check if field already exists
    int idx = xr_shape_field_index(shape, symbol);
    if (idx >= 0) {
        // Existing field: update in-object or overflow
        if (idx < shape->in_object_capacity) {
            json->fields[idx] = value;
        } else {
            XrJsonOverflow *ov = json->overflow;
            if (ov) {
                uint16_t ov_idx = (uint16_t)(idx - shape->in_object_capacity);
                if (ov_idx < ov->capacity) {
                    ov->values[ov_idx] = value;
                }
            }
        }
        XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), json);
        return;
    }

    // Field doesn't exist: try Shape transition
    XrShape *new_shape = xr_shape_transition(X, shape, symbol);

    if (new_shape) {
        // Zero-copy transition: just swap shape_id
        xr_json_set_shape(json, new_shape);
        int new_idx = new_shape->field_count - 1;
        if (new_idx < shape->in_object_capacity) {
            // Fits in-object
            json->fields[new_idx] = value;
        } else {
            // Overflow: allocate/grow overflow array
            uint16_t ov_idx = (uint16_t)(new_idx - shape->in_object_capacity);
            XrJsonOverflow *ov = json->overflow;
            if (!ov || ov_idx >= ov->capacity) {
                ov = overflow_grow(ov, ov_idx + 1);
                json->overflow = ov;
            }
            if (ov) {
                ov->values[ov_idx] = value;
                if (ov_idx >= ov->length) {
                    ov->length = ov_idx + 1;
                }
            }
        }
    }
    XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), json);
}

// Get field by string key
XrValue xr_json_get_by_key(XrayIsolate *X, XrJson *json, const char *key) {
    XR_DCHECK(X != NULL, "json_get_by_key: NULL isolate");
    if (!json || !key) return xr_null();

    // Intern key as Symbol
    XrSymbolTable *table = get_symbol_table(X);
    SymbolId symbol = xr_symbol_register_in_table(table, key);

    return xr_json_get(X, json, symbol);
}

// Set field by string key
void xr_json_set_by_key(XrayIsolate *X, XrJson *json, const char *key, XrValue value) {
    XR_DCHECK(X != NULL, "json_set_by_key: NULL isolate");
    if (!json || !key) return;

    // Intern key as Symbol
    XrSymbolTable *table = get_symbol_table(X);
    SymbolId symbol = xr_symbol_register_in_table(table, key);

    xr_json_set(X, json, symbol, value);
}

/* ========== GC Related Implementation ========== */

// Destructor: release overflow malloc memory when Json is collected
void xr_gc_destroy_json(XrGCHeader *obj, struct XrCoroGC *owning_gc) {
    (void)owning_gc;
    XrJson *json = (XrJson*)obj;
    if (json && json->overflow) {
        xr_free(json->overflow);
        json->overflow = NULL;
    }
}

