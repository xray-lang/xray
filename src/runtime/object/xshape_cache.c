/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xshape_cache.c - Shape cache implementation
 */

#include "xshape_cache.h"
#include "../../base/xchecks.h"
#include "xstring.h"
#include "../symbol/xsymbol_table.h"
#include "../xisolate_api.h"
#include "../../base/xmalloc.h"
#include "../../base/xhash.h"
#include <string.h>
#include <stdlib.h>

static inline XrSymbolTable *get_symbol_table(XrayIsolate *isolate) {
    return (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
}

// Hash interned strings by pointer (O(1) comparison)
static uint32_t compute_hash_interned(XrString **interned_names, uint32_t field_count) {
    XR_DCHECK(field_count == 0 || interned_names != NULL, "compute_hash_interned: NULL names");
    uint32_t hash = XR_FNV_OFFSET_BASIS;

    for (uint32_t i = 0; i < field_count; i++) {
        uintptr_t ptr = (uintptr_t) interned_names[i];
        hash ^= (uint32_t) (ptr & 0xFFFFFFFF);
        hash *= XR_FNV_PRIME;
        hash ^= (uint32_t) (ptr >> 32);
        hash *= XR_FNV_PRIME;
    }

    return hash;
}

static int shape_fields_match(XrShape *shape, XrString **interned_names, uint32_t field_count,
                              XrayIsolate *isolate) {
    XR_DCHECK(shape != NULL, "shape_fields_match: NULL shape");
    if (shape->field_count != field_count)
        return 0;
    if (field_count == 0)
        return 1;

    XrSymbolTable *table = get_symbol_table(isolate);

    for (uint32_t i = 0; i < field_count; i++) {
        const char *name = interned_names[i]->data;
        SymbolId expected_sym = xr_symbol_register_in_table(table, name);
        if (shape->field_symbols[i] != expected_sym) {
            return 0;
        }
    }

    return 1;
}

XrShapeCache *xr_shape_cache_new(void) {
    XrShapeCache *cache = (XrShapeCache *) xr_malloc(sizeof(XrShapeCache));
    if (!cache)
        return NULL;

    memset(cache->buckets, 0, sizeof(cache->buckets));
    cache->hit_count = 0;
    cache->miss_count = 0;

    return cache;
}

// Note: Does not free Shapes (GC handles them)
void xr_shape_cache_free(XrShapeCache *cache) {
    if (!cache)
        return;

    for (int i = 0; i < SHAPE_CACHE_SIZE; i++) {
        XrShapeCacheEntry *entry = cache->buckets[i];
        while (entry) {
            XrShapeCacheEntry *next = entry->next;
            xr_free(entry);
            entry = next;
        }
    }

    xr_free(cache);
}

XrShape *xr_shape_cache_get_or_create(XrayIsolate *isolate, XrShapeCache *cache,
                                      XrString **interned_names, uint32_t field_count) {
    if (!cache)
        return NULL;

    uint32_t hash = compute_hash_interned(interned_names, field_count);
    int bucket = hash % SHAPE_CACHE_SIZE;

    // Search existing entries
    XrShapeCacheEntry *entry = cache->buckets[bucket];
    while (entry) {
        if (entry->hash == hash &&
            shape_fields_match(entry->shape, interned_names, field_count, isolate)) {
            cache->hit_count++;
            return entry->shape;
        }
        entry = entry->next;
    }

    // Cache miss - create new Shape
    cache->miss_count++;

    // Compile-time known structures: tight capacity with modest padding.
    // For {left, right}, capacity=6, saving 64B per object vs padding=8.
    // Fields beyond capacity spill to XrJsonOverflow (malloc-backed).
    uint16_t capacity = (uint16_t) field_count;
    if (capacity == 0)
        capacity = 1;
    if (capacity > SHAPE_MAX_FAST_FIELDS) {
        capacity = SHAPE_MAX_FAST_FIELDS;
    }
    XrShape *shape = xr_shape_new(isolate, capacity);
    if (!shape)
        return NULL;

    // Add fields one by one (build transition chain)
    XrSymbolTable *table = get_symbol_table(isolate);
    XrShape *current = shape;

    for (uint32_t i = 0; i < field_count; i++) {
        const char *name = interned_names[i]->data;
        SymbolId sym = xr_symbol_register_in_table(table, name);

        XrShape *next = xr_shape_transition(isolate, current, sym);
        if (!next) {
            return NULL;
        }
        current = next;
    }

    entry = (XrShapeCacheEntry *) xr_malloc(sizeof(XrShapeCacheEntry));
    if (!entry)
        return current;

    entry->hash = hash;
    entry->shape = current;
    entry->next = cache->buckets[bucket];
    cache->buckets[bucket] = entry;

    return current;
}
