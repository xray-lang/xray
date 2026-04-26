/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xic_field.c - Field access inline cache implementation
 */

#include "xic_field.h"
#include "../base/xchecks.h"
#include <stdio.h>
#include <string.h>

void xr_ic_field_update(XrICField *ic, XrClass *cls, int offset, int symbol) {
    if (!ic || !cls)
        return;

    // Reset cache if symbol changed (different field access at same instruction)
    if (ic->cached_symbol != -1 && ic->cached_symbol != symbol) {
        xr_ic_field_reset(ic);
    }

    ic->cached_symbol = symbol;

    switch (ic->state) {
        case XR_IC_FIELD_UNINIT:
            // First access -> monomorphic
            ic->entries[0].cls = cls;
            ic->entries[0].offset = offset;
            ic->entries[0].hit_count = 0;
            ic->entry_count = 1;
            ic->state = XR_IC_FIELD_MONO;
            break;

        case XR_IC_FIELD_MONO:
            if (ic->entries[0].cls == cls) {
                ic->entries[0].offset = offset;
            } else {
                // Different class -> polymorphic
                ic->entries[1].cls = cls;
                ic->entries[1].offset = offset;
                ic->entries[1].hit_count = 0;
                ic->entry_count = 2;
                ic->state = XR_IC_FIELD_POLY;
            }
            break;

        case XR_IC_FIELD_POLY:
            for (int i = 0; i < ic->entry_count; i++) {
                if (ic->entries[i].cls == cls) {
                    ic->entries[i].offset = offset;
                    return;
                }
            }

            if (ic->entry_count < 4) {
                ic->entries[ic->entry_count].cls = cls;
                ic->entries[ic->entry_count].offset = offset;
                ic->entries[ic->entry_count].hit_count = 0;
                ic->entry_count++;
            } else {
                // >4 types -> megamorphic
                ic->state = XR_IC_FIELD_MEGA;
                ic->miss_count++;
            }
            break;

        case XR_IC_FIELD_MEGA:
            ic->miss_count++;
            break;
    }
}

void xr_ic_field_reset(XrICField *ic) {
    if (!ic)
        return;
    xr_ic_field_init(ic);
}

void xr_ic_field_collect_stats(XrICField *ic, XrICFieldStats *stats) {
    if (!ic || !stats)
        return;

    switch (ic->state) {
        case XR_IC_FIELD_UNINIT:
            break;

        case XR_IC_FIELD_MONO:
            stats->mono_hits += ic->entries[0].hit_count;
            break;

        case XR_IC_FIELD_POLY:
            for (int i = 0; i < ic->entry_count; i++) {
                stats->poly_hits += ic->entries[i].hit_count;
            }
            break;

        case XR_IC_FIELD_MEGA:
            stats->mega_accesses += ic->miss_count;
            break;
    }

    stats->misses += ic->miss_count;
}

void xr_ic_field_print_stats(XrICFieldStats *stats) {
    if (!stats)
        return;

    uint64_t total = stats->mono_hits + stats->poly_hits + stats->misses;

    if (total == 0) {
        printf("[FieldIC] No accesses\n");
        return;
    }

    printf("[FieldIC] Statistics:\n");
    printf("  Monomorphic hits: %llu (%.1f%%)\n", (unsigned long long) stats->mono_hits,
           100.0 * stats->mono_hits / total);
    printf("  Polymorphic hits: %llu (%.1f%%)\n", (unsigned long long) stats->poly_hits,
           100.0 * stats->poly_hits / total);
    printf("  Misses: %llu (%.1f%%)\n", (unsigned long long) stats->misses,
           100.0 * stats->misses / total);
    printf("  Megamorphic accesses: %llu\n", (unsigned long long) stats->mega_accesses);
    printf("  Total: %llu\n", (unsigned long long) total);

    uint64_t hits = stats->mono_hits + stats->poly_hits;
    printf("  Hit rate: %.1f%%\n", 100.0 * hits / total);
}
