/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xic_field_table.h - Field access IC table management
 *
 * KEY CONCEPT:
 *   Manages a dynamic array of field inline caches for a prototype.
 */

#ifndef XIC_FIELD_TABLE_H
#define XIC_FIELD_TABLE_H

#include "xic_field.h"

typedef struct XrICFieldTable {
    XrICField *caches;
    int count;
    int capacity;
} XrICFieldTable;

XR_FUNC XrICFieldTable *xr_ic_field_table_new(int initial_capacity);
XR_FUNC void xr_ic_field_table_free(XrICFieldTable *table);
XR_FUNC int xr_ic_field_table_alloc(XrICFieldTable *table);

static inline XrICField *xr_ic_field_table_get(XrICFieldTable *table, int index) {
    if (!table || index < 0 || index >= table->count) {
        return NULL;
    }
    return &table->caches[index];
}

#endif  // XIC_FIELD_TABLE_H
