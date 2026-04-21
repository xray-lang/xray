/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson_pool.c - Json object pool implementation
 */

#include "xjson_pool.h"
#include "../../base/xchecks.h"
#include "xjson.h"
#include "xshape.h"
#include "../gc/xgc.h"
#include "../../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../xisolate_api.h"

/* ========== Internal Functions ========== */

// Get Json object's field count
static inline int json_get_field_count(struct XrJson *json) {
    if (!json) return 0;
    XrShape *s = xr_json_shape(xray_isolate_current(), json);
    return s ? s->in_object_capacity : 0;
}

/* ========== API Implementation ========== */

void xr_json_pool_init(XrJsonPoolManager *pm) {
    if (!pm) return;

    memset(pm, 0, sizeof(XrJsonPoolManager));
    pm->enabled = true;

    // Initialize pools for each field count
    for (int i = 0; i < XR_JSON_POOL_MAX_FIELDS; i++) {
        pm->pools[i].field_count = i;
        pm->pools[i].slots = NULL;
        pm->pools[i].count = 0;
        pm->pools[i].capacity = 0;
    }
}

void xr_json_pool_cleanup(XrJsonPoolManager *pm) {
    if (!pm) return;

    for (int i = 0; i < XR_JSON_POOL_MAX_FIELDS; i++) {
        XrJsonPool *pool = &pm->pools[i];

        if (pool->slots) {
            // Free all objects in pool
            for (int j = 0; j < pool->count; j++) {
                if (pool->slots[j]) {
                    xr_free(pool->slots[j]);
                }
            }
            xr_free(pool->slots);
            pool->slots = NULL;
        }
        pool->count = 0;
        pool->capacity = 0;
    }
}

struct XrJson* xr_json_pool_get(XrJsonPoolManager *pm, struct XrShape *shape, int field_count) {
    XR_DCHECK(field_count >= 0, "json_pool_get: negative field_count");
    if (!pm || !pm->enabled) return NULL;

    pm->total_allocs++;

    // Check if poolable
    if (field_count >= XR_JSON_POOL_MAX_FIELDS) {
        pm->heap_allocs++;
        return NULL;  // Too many fields, skip pooling
    }

    XrJsonPool *pool = &pm->pools[field_count];

    // Try to get from pool
    if (pool->count > 0) {
        pool->count--;
        struct XrJson *json = pool->slots[pool->count];
        pool->slots[pool->count] = NULL;

        // Check if pooled object has enough capacity
        int actual_capacity = json_get_field_count(json);
        if (actual_capacity < shape->in_object_capacity) {
            // Not enough capacity, cannot reuse
            pool->misses++;
            pm->heap_allocs++;
            xr_free(json);
            return NULL;
        }

        pool->hits++;
        pm->pool_allocs++;

        // Reinitialize GC header
        xr_gc_header_init_type(&json->gc, XR_TJSON);
        xr_json_set_shape(json, shape);

        // Release any overflow from previous use
        if (json->overflow) {
            xr_free(json->overflow);
            json->overflow = NULL;
        }

        // Initialize fields to null
        for (int i = 0; i < shape->in_object_capacity; i++) {
            json->fields[i] = xr_null();
        }

        return json;
    }

    // Pool empty, need new allocation
    pool->misses++;
    pm->heap_allocs++;
    return NULL;
}

bool xr_json_pool_return(XrJsonPoolManager *pm, struct XrJson *json) {
    if (!pm || !json || !pm->enabled) return false;

    int field_count = json_get_field_count(json);

    // Check if poolable
    if (field_count >= XR_JSON_POOL_MAX_FIELDS) {
        return false;  // Too many fields
    }

    XrJsonPool *pool = &pm->pools[field_count];

    // Check if pool is full
    if (pool->count >= XR_JSON_POOL_MAX_SIZE) {
        pool->overflows++;
        return false;  // Pool full, needs to be freed
    }

    // Expand pool capacity if needed
    if (pool->count >= pool->capacity) {
        int new_capacity = pool->capacity == 0 ? 64 : pool->capacity * 2;
        if (new_capacity > XR_JSON_POOL_MAX_SIZE) {
            new_capacity = XR_JSON_POOL_MAX_SIZE;
        }

        struct XrJson **new_slots = (struct XrJson**)xr_realloc(
            pool->slots,
            new_capacity * sizeof(struct XrJson*)
        );
        if (!new_slots) {
            return false;  // Allocation failed
        }

        pool->slots = new_slots;
        pool->capacity = new_capacity;
    }

    // Return to pool
    pool->slots[pool->count] = json;
    pool->count++;
    pool->returns++;

    return true;
}

void xr_json_pool_dump_stats(XrJsonPoolManager *pm) {
    if (!pm) return;

    printf("\n=== Json Pool Statistics ===\n");
    printf("Enabled: %s\n", pm->enabled ? "yes" : "no");
    printf("Total allocs: %llu\n", pm->total_allocs);
    printf("Pool allocs:  %llu (%.1f%%)\n", pm->pool_allocs,
           pm->total_allocs > 0 ? 100.0 * pm->pool_allocs / pm->total_allocs : 0);
    printf("Heap allocs:  %llu (%.1f%%)\n", pm->heap_allocs,
           pm->total_allocs > 0 ? 100.0 * pm->heap_allocs / pm->total_allocs : 0);

    printf("\nPer-field-count pools:\n");
    for (int i = 0; i < XR_JSON_POOL_MAX_FIELDS; i++) {
        XrJsonPool *pool = &pm->pools[i];
        if (pool->hits > 0 || pool->misses > 0 || pool->count > 0) {
            printf("  [%d fields] cached=%d hits=%llu misses=%llu returns=%llu overflows=%llu\n",
                   i, pool->count, pool->hits, pool->misses, pool->returns, pool->overflows);
        }
    }
    printf("\n");
}

void xr_json_pool_set_enabled(XrJsonPoolManager *pm, bool enabled) {
    if (!pm) return;
    pm->enabled = enabled;
}
