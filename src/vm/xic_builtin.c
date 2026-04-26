/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xic_builtin.c - Builtin-method inline cache implementation.
 *
 * Mirrors the construction / teardown shape of xic_method.c so the
 * per-coroutine bookkeeping in xvm_ic.c can grow all three IC kinds
 * (field / method / builtin) in lockstep.
 */

#include "xic_builtin.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include <string.h>

static void ic_builtin_init(XrICBuiltin *ic) {
    /* slot == NULL is the empty sentinel; cached_tid is meaningless
     * until slot is non-NULL. Zero-init is sufficient. */
    memset(ic, 0, sizeof(*ic));
}

XrICBuiltinTable *xr_ic_builtin_table_new(int initial_capacity) {
    XrICBuiltinTable *table =
        (XrICBuiltinTable *)xr_malloc(sizeof(XrICBuiltinTable));
    if (!table) return NULL;

    table->capacity = initial_capacity > 0 ? initial_capacity : 4;
    table->count = 0;
    table->caches =
        (XrICBuiltin *)xr_malloc(sizeof(XrICBuiltin) * (size_t)table->capacity);
    if (!table->caches) {
        xr_free(table);
        return NULL;
    }
    for (int i = 0; i < table->capacity; i++) {
        ic_builtin_init(&table->caches[i]);
    }
    return table;
}

void xr_ic_builtin_table_free(XrICBuiltinTable *table) {
    if (!table) return;
    if (table->caches) xr_free(table->caches);
    xr_free(table);
}

int xr_ic_builtin_table_alloc(XrICBuiltinTable *table) {
    XR_DCHECK(table != NULL, "ic_builtin_table_alloc: NULL table");
    XR_DCHECK(table->count >= 0 && table->count <= table->capacity,
              "ic_builtin_table_alloc: count/capacity invariant violated");

    if (table->count >= table->capacity) {
        int new_cap = table->capacity * 2;
        XrICBuiltin *new_caches = (XrICBuiltin *)xr_realloc(
            table->caches, sizeof(XrICBuiltin) * (size_t)new_cap);
        if (!new_caches) return -1;
        table->caches = new_caches;
        for (int i = table->capacity; i < new_cap; i++) {
            ic_builtin_init(&table->caches[i]);
        }
        table->capacity = new_cap;
    }
    return table->count++;
}
