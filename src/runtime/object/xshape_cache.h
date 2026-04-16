/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xshape_cache.h - Shape cache system
 *
 * KEY CONCEPT:
 *   Caches XrShape for same field set.
 *   Uses interned strings for O(1) pointer comparison.
 */

#ifndef XSHAPE_CACHE_H
#define XSHAPE_CACHE_H

#include "xshape.h"
#include "xstring.h"
#include <stdint.h>

#define SHAPE_CACHE_SIZE 64

typedef struct XrShapeCacheEntry {
    uint32_t hash;
    XrShape *shape;
    struct XrShapeCacheEntry *next;
} XrShapeCacheEntry;

typedef struct XrShapeCache {
    XrShapeCacheEntry *buckets[SHAPE_CACHE_SIZE];
    int hit_count;
    int miss_count;
} XrShapeCache;

XR_FUNC XrShapeCache* xr_shape_cache_new(void);
XR_FUNC void xr_shape_cache_free(XrShapeCache *cache);

XR_FUNC XrShape* xr_shape_cache_get_or_create(
    XrayIsolate *isolate,
    XrShapeCache *cache,
    XrString **interned_names,
    uint32_t field_count
);

#endif // XSHAPE_CACHE_H
