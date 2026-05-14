/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xic_method.c - Method call inline cache implementation
 */

#include "xic_method.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include <stdio.h>
#include <string.h>

/* ========== Inline Cache Table Operations ========== */

static void ic_method_init(XrICMethod *ic) {
    if (!ic)
        return;
    memset(ic->entries, 0, sizeof(ic->entries));
    ic->count = 0;
    ic->is_megamorphic = 0;
    ic->total_count = 0;
    ic->mega_cache = NULL;
#ifndef NDEBUG
    ic->debug_instruction_offset = -1;
#endif
#ifdef XR_DEBUG_INLINE_CACHE
    ic->hits = 0;
    ic->misses = 0;
#endif
}

XrICMethodTable *xr_ic_method_table_new(int initial_capacity) {
    XrICMethodTable *table = (XrICMethodTable *) xr_malloc(sizeof(XrICMethodTable));
    if (!table)
        return NULL;
    table->capacity = initial_capacity > 0 ? initial_capacity : 4;
    table->count = 0;
    table->caches = (XrICMethod *) xr_malloc(sizeof(XrICMethod) * table->capacity);

    for (int i = 0; i < table->capacity; i++) {
        ic_method_init(&table->caches[i]);
    }

    return table;
}

void xr_ic_method_table_free(XrICMethodTable *table) {
    if (!table)
        return;

    if (table->caches) {
        for (int i = 0; i < table->count; i++) {
            if (table->caches[i].mega_cache) {
                xr_free(table->caches[i].mega_cache);
            }
        }
        xr_free(table->caches);
    }

    xr_free(table);
}

int xr_ic_method_table_alloc(XrICMethodTable *table) {
    XR_DCHECK(table != NULL, "Inline cache table must not be NULL");
    XR_DCHECK(table->count >= 0 && table->count <= table->capacity,
              "ic_method_table_alloc: count/capacity invariant violated");

    if (table->count >= table->capacity) {
        int new_capacity = table->capacity * 2;
        XrICMethod *new_caches =
            (XrICMethod *) xr_realloc(table->caches, sizeof(XrICMethod) * new_capacity);
        if (!new_caches)
            return -1;
        table->caches = new_caches;

        for (int i = table->capacity; i < new_capacity; i++) {
            ic_method_init(&table->caches[i]);
        }

        table->capacity = new_capacity;
    }

    return table->count++;
}

/* ========== Type Feedback Dump (always available) ========== */

static const char *ic_state_name(XrICState state) {
    switch (state) {
        case XR_IC_STATE_UNINIT:
            return "uninit";
        case XR_IC_STATE_MONO:
            return "mono";
        case XR_IC_STATE_POLY:
            return "poly";
        case XR_IC_STATE_MEGA:
            return "mega";
        default:
            return "?";
    }
}

void xr_ic_method_dump_feedback(const XrICMethod *cache, int pc_offset, const char *func_name) {
    (void) func_name;
    if (!cache || cache->total_count == 0)
        return;

    XrICState state = xr_ic_method_state(cache);
    fprintf(stderr, "  [pc=%04d] %s  total=%u  types=%d", pc_offset, ic_state_name(state),
            cache->total_count, (int) cache->count);

    for (int i = 0; i < cache->count; i++) {
        const XrICEntry *e = &cache->entries[i];
        double pct = cache->total_count > 0 ? 100.0 * e->hit_count / cache->total_count : 0.0;
        fprintf(stderr, "  %s:%.1f%%", e->klass ? xr_class_display_name(e->klass) : "?", pct);
    }
    fprintf(stderr, "\n");
}

void xr_ic_method_table_dump_feedback(const XrICMethodTable *table, const char *func_name) {
    if (!table)
        return;

    int active = 0;
    for (int i = 0; i < table->count; i++) {
        if (table->caches[i].total_count > 0)
            active++;
    }
    if (active == 0)
        return;

    fprintf(stderr, "[ic-feedback] %s: %d active call sites\n", func_name ? func_name : "?",
            active);

    for (int i = 0; i < table->count; i++) {
        xr_ic_method_dump_feedback(&table->caches[i], i, func_name);
    }
}

/* ========== Statistics and Debug ========== */

#ifdef XR_DEBUG_INLINE_CACHE

void xr_ic_method_print_stats(const XrICMethod *cache, const char *name) {
    if (!cache) {
        printf("Cache [%s]: NULL\n", name ? name : "unnamed");
        return;
    }

    uint64_t total = cache->hits + cache->misses;
    double hit_rate = total > 0 ? (100.0 * cache->hits / total) : 0.0;

    printf("Cache [%s]:\n", name ? name : "unnamed");
    printf("  Entries:  %d/%d%s\n", cache->count, XR_IC_POLY_MAX,
           cache->is_megamorphic ? " (megamorphic)" : "");
    printf("  Hits:     %llu\n", cache->hits);
    printf("  Misses:   %llu\n", cache->misses);
    printf("  Hit Rate: %.2f%%\n", hit_rate);

    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].klass) {
            printf("  [%d] class=%s\n", i, xr_class_display_name(cache->entries[i].klass));
        }
    }
}

void xr_ic_method_table_print_stats(const XrICMethodTable *table) {
    if (!table) {
        printf("XrICMethodTable: NULL\n");
        return;
    }

    printf("\n========== Inline Cache Statistics ==========\n");
    printf("Cache count: %d / %d\n", table->count, table->capacity);

    uint64_t total_hits = 0;
    uint64_t total_misses = 0;

    for (int i = 0; i < table->count; i++) {
        total_hits += table->caches[i].hits;
        total_misses += table->caches[i].misses;
    }

    uint64_t total = total_hits + total_misses;
    double overall_hit_rate = total > 0 ? (100.0 * total_hits / total) : 0.0;

    printf("Total hits:   %llu\n", total_hits);
    printf("Total misses: %llu\n", total_misses);
    printf("Overall hit rate: %.2f%%\n", overall_hit_rate);
    printf("==============================================\n\n");

    for (int i = 0; i < table->count; i++) {
        char name[32];
        snprintf(name, sizeof(name), "Cache #%d", i);
        xr_ic_method_print_stats(&table->caches[i], name);
        printf("\n");
    }
}

double xr_ic_method_hit_rate(const XrICMethod *cache) {
    if (!cache)
        return 0.0;

    uint64_t total = cache->hits + cache->misses;
    return total > 0 ? ((double) cache->hits / total) : 0.0;
}

#endif
