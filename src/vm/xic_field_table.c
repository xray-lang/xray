/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xic_field_table.c - Field access IC table implementation
 */

#include "xic_field_table.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

XrICFieldTable *xr_ic_field_table_new(int initial_capacity) {
    XR_DCHECK(initial_capacity >= 0, "ic_field_table_new: negative capacity");
    XrICFieldTable *table = (XrICFieldTable *) xr_malloc(sizeof(XrICFieldTable));
    if (!table)
        return NULL;

    table->capacity = initial_capacity > 0 ? initial_capacity : 4;
    table->count = 0;
    table->caches = (XrICField *) xr_malloc(sizeof(XrICField) * table->capacity);
    if (!table->caches) {
        xr_free(table);
        return NULL;
    }

    for (int i = 0; i < table->capacity; i++) {
        xr_ic_field_init(&table->caches[i]);
    }

    return table;
}

void xr_ic_field_table_free(XrICFieldTable *table) {
    if (!table)
        return;

    if (table->caches) {
        xr_free(table->caches);
    }

    xr_free(table);
}

int xr_ic_field_table_alloc(XrICFieldTable *table) {
    if (!table)
        return -1;

    if (table->count >= table->capacity) {
        int new_capacity = table->capacity * 2;
        XrICField *new_caches =
            (XrICField *) xr_realloc(table->caches, sizeof(XrICField) * new_capacity);
        if (!new_caches)
            return -1;
        table->caches = new_caches;

        for (int i = table->capacity; i < new_capacity; i++) {
            xr_ic_field_init(&table->caches[i]);
        }

        table->capacity = new_capacity;
    }

    return table->count++;
}
