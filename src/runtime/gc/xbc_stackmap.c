/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbc_stackmap.c - Bytecode stack map builder and lifecycle
 */

#include "xbc_stackmap.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>
#include <stdlib.h>

/* ========== Builder Internal ========== */

#define BC_SM_INIT_CAP 16

typedef struct {
    uint32_t pc;
    uint64_t *bitmap;  // Owned copy, num_words words
    uint16_t num_words;
    uint16_t live_count;
} BuilderEntry;

struct XrBcStackMapBuilder {
    BuilderEntry *entries;
    uint32_t count;
    uint32_t capacity;
    uint16_t maxslots;
    uint16_t num_words;  // (maxslots + 63) / 64
};

/* ========== Builder API ========== */

XrBcStackMapBuilder *xr_bc_stackmap_builder_create(uint16_t maxslots) {
    XrBcStackMapBuilder *b = xr_malloc(sizeof(XrBcStackMapBuilder));
    if (!b)
        return NULL;
    b->entries = NULL;
    b->count = 0;
    b->capacity = 0;
    b->maxslots = maxslots;
    b->num_words = (maxslots + 63) / 64;
    return b;
}

void xr_bc_stackmap_builder_add(XrBcStackMapBuilder *b, uint32_t pc, const uint64_t *live_bitmap) {
    XR_DCHECK(b != NULL, "stackmap_builder_add: NULL builder");
    XR_DCHECK(live_bitmap != NULL, "stackmap_builder_add: NULL bitmap");

    // Grow entries array if needed
    if (b->count >= b->capacity) {
        uint32_t newcap = b->capacity ? b->capacity * 2 : BC_SM_INIT_CAP;
        BuilderEntry *tmp = xr_realloc(b->entries, newcap * sizeof(BuilderEntry));
        if (!tmp)
            return;  // OOM: silently skip (conservative scan fallback)
        b->entries = tmp;
        b->capacity = newcap;
    }

    // Copy bitmap
    size_t bm_bytes = (size_t) b->num_words * sizeof(uint64_t);
    uint64_t *bm_copy = xr_malloc(bm_bytes);
    if (!bm_copy)
        return;
    memcpy(bm_copy, live_bitmap, bm_bytes);

    // Count live slots
    uint16_t live = 0;
    for (uint16_t w = 0; w < b->num_words; w++) {
        live += (uint16_t) __builtin_popcountll(bm_copy[w]);
    }

    BuilderEntry *e = &b->entries[b->count++];
    e->pc = pc;
    e->bitmap = bm_copy;
    e->num_words = b->num_words;
    e->live_count = live;
}

// qsort comparator for BuilderEntry by pc
static int entry_cmp(const void *a, const void *b) {
    uint32_t pa = ((const BuilderEntry *) a)->pc;
    uint32_t pb = ((const BuilderEntry *) b)->pc;
    return (pa > pb) - (pa < pb);
}

XrBcStackMap *xr_bc_stackmap_builder_finish(XrBcStackMapBuilder *b) {
    XR_DCHECK(b != NULL, "stackmap_builder_finish: NULL builder");

    if (b->count == 0) {
        // No safepoints recorded — free builder, return NULL
        xr_free(b->entries);
        xr_free(b);
        return NULL;
    }

    // Sort entries by pc
    qsort(b->entries, b->count, sizeof(BuilderEntry), entry_cmp);

    // Phase 1: compute bitmap pool size (simple: no dedup for now)
    uint32_t total_words = 0;
    for (uint32_t i = 0; i < b->count; i++) {
        total_words += b->entries[i].num_words;
    }

    // Allocate result
    XrBcStackMap *map = xr_malloc(sizeof(XrBcStackMap));
    if (!map)
        goto fail;

    map->entries = xr_malloc(b->count * sizeof(XrBcStackMapEntry));
    if (!map->entries) {
        xr_free(map);
        map = NULL;
        goto fail;
    }

    map->bitmap_pool = xr_malloc((size_t) total_words * sizeof(uint64_t));
    if (!map->bitmap_pool) {
        xr_free(map->entries);
        xr_free(map);
        map = NULL;
        goto fail;
    }

    map->count = b->count;
    map->maxslots = b->maxslots;
    map->bitmap_pool_words = total_words;
    map->_pad = 0;

    // Phase 2: pack entries and bitmaps
    uint32_t pool_offset = 0;
    for (uint32_t i = 0; i < b->count; i++) {
        BuilderEntry *be = &b->entries[i];
        XrBcStackMapEntry *me = &map->entries[i];
        me->pc = be->pc;
        me->bitmap_offset = pool_offset;
        me->live_count = be->live_count;
        me->num_words = be->num_words;

        memcpy(&map->bitmap_pool[pool_offset], be->bitmap,
               (size_t) be->num_words * sizeof(uint64_t));
        pool_offset += be->num_words;
    }

    XR_DCHECK(pool_offset == total_words, "bitmap pool offset mismatch");

fail:
    // Free builder resources
    for (uint32_t i = 0; i < b->count; i++) {
        xr_free(b->entries[i].bitmap);
    }
    xr_free(b->entries);
    xr_free(b);
    return map;
}

void xr_bc_stackmap_destroy(XrBcStackMap *map) {
    if (!map)
        return;
    xr_free(map->entries);
    xr_free(map->bitmap_pool);
    xr_free(map);
}
