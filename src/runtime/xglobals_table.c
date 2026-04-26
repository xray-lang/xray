/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xglobals_table.c - Dynamic global variable table implementation
 */

#include "xglobals_table.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include <string.h>
#include <stdio.h>

#define GLOBALS_MIN_CAPACITY 16
#define GLOBALS_GROWTH_FACTOR 2

XrGlobalsTable* xr_globals_create(size_t initial_capacity) {
    XrGlobalsTable *globals = (XrGlobalsTable*)xr_malloc(sizeof(XrGlobalsTable));
    if (!globals) return NULL;
    if (initial_capacity < GLOBALS_MIN_CAPACITY) initial_capacity = GLOBALS_MIN_CAPACITY;
    globals->values = (XrValue*)xr_malloc(sizeof(XrValue) * initial_capacity);
    if (!globals->values) { xr_free(globals); return NULL; }
    globals->capacity = initial_capacity;
    globals->count = 0;
    for (size_t i = 0; i < initial_capacity; i++) globals->values[i] = xr_null();
    return globals;
}

void xr_globals_destroy(XrGlobalsTable *globals) {
    if (!globals) return;
    // The XrValue entries reference heap objects (enum types, classes,
    // closures, etc.) but globals does NOT own those bodies. Each
    // pointee participates in its own owner protocol — fixedgc, system
    // heap, or shared/refcount — and is released by the corresponding
    // cleanup path (xr_gc_cleanup, xr_sysheap_destroy, xr_shared_decref).
    // Releasing the values array here is the full extent of this owner.
    if (globals->values) xr_free(globals->values);
    xr_free(globals);
}

bool xr_globals_resize(XrGlobalsTable *globals, size_t new_capacity) {
    if (!globals) return false;
    if (new_capacity < globals->count) new_capacity = globals->count;
    if (new_capacity < GLOBALS_MIN_CAPACITY) new_capacity = GLOBALS_MIN_CAPACITY;
    XrValue *new_values = (XrValue*)xr_realloc(globals->values, sizeof(XrValue) * new_capacity);
    if (!new_values) return false;
    for (size_t i = globals->capacity; i < new_capacity; i++) new_values[i] = xr_null();
    globals->values = new_values;
    globals->capacity = new_capacity;
    return true;
}

int xr_globals_add(XrGlobalsTable *globals, XrValue value) {
    if (!globals) return -1;
    if (globals->count >= globals->capacity) {
        if (!xr_globals_resize(globals, globals->capacity * GLOBALS_GROWTH_FACTOR)) return -1;
    }
    int index = (int)globals->count;
    globals->values[index] = value;
    globals->count++;
    XR_DCHECK(globals->count <= globals->capacity, "globals_add: count > capacity");
    return index;
}

XrValue xr_globals_get(XrGlobalsTable *globals, int index) {
    if (!globals || index < 0 || (size_t)index >= globals->count) return xr_null();
    return globals->values[index];
}

bool xr_globals_set(XrGlobalsTable *globals, int index, XrValue value) {
    if (!globals || index < 0) return false;
    if ((size_t)index >= globals->capacity) {
        if (!xr_globals_resize(globals, ((size_t)index + 1) * GLOBALS_GROWTH_FACTOR)) return false;
    }
    globals->values[index] = value;
    if ((size_t)index >= globals->count) globals->count = (size_t)index + 1;
    return true;
}

size_t xr_globals_count(XrGlobalsTable *globals) {
    return globals ? globals->count : 0;
}
