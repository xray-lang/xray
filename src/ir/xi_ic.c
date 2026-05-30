/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_ic.c - IC snapshot metadata attachment for Xi IR
 *
 * Reads VM inline cache tables and attaches classification
 * metadata to each call-site value in the Xi IR function.
 */

#include "xi_ic.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../vm/xic_field_table.h"
#include "../vm/xic_method.h"
#include <string.h>

/* ========== Table Management ========== */

static XiIcTable *table_new(uint32_t initial_cap) {
    XiIcTable *t = (XiIcTable *) xr_calloc(1, sizeof(XiIcTable));
    if (!t)
        return NULL;
    if (initial_cap > 0) {
        t->entries = (XiIcMeta *) xr_calloc(initial_cap, sizeof(XiIcMeta));
        if (!t->entries) {
            xr_free(t);
            return NULL;
        }
        t->capacity = initial_cap;
    }
    return t;
}

static bool table_add(XiIcTable *t, const XiIcMeta *meta) {
    XR_DCHECK(t != NULL, "table_add: NULL table");
    if (t->nentries >= t->capacity) {
        uint32_t new_cap = t->capacity ? t->capacity * 2 : 8;
        XiIcMeta *grown = (XiIcMeta *) xr_realloc(t->entries, new_cap * sizeof(XiIcMeta));
        if (!grown)
            return false;
        t->entries = grown;
        t->capacity = new_cap;
    }
    t->entries[t->nentries++] = *meta;
    return true;
}

XR_FUNC void xi_ic_table_free(XiIcTable *table) {
    if (!table)
        return;
    xr_free(table->entries);
    xr_free(table);
}

/* ========== IC Classification ========== */

static XiIcKind classify_method_ic(const XrICMethod *ic) {
    if (!ic || ic->count == 0)
        return XI_IC_NONE;
    if (ic->is_megamorphic)
        return XI_IC_MEGA;
    if (ic->count == 1)
        return XI_IC_MONO;
    return XI_IC_POLY;
}

static XiIcKind classify_field_ic(const XrICField *ic) {
    if (!ic)
        return XI_IC_NONE;
    if (ic->state == XR_IC_FIELD_UNINIT || ic->entry_count == 0)
        return XI_IC_NONE;
    if (ic->state == XR_IC_FIELD_MEGA)
        return XI_IC_MEGA;
    if (ic->state == XR_IC_FIELD_MONO)
        return XI_IC_MONO;
    return XI_IC_POLY;
}

/* ========== Attach Pass ========== */

static bool is_method_call_site(uint16_t op) {
    return op == XI_CALL_METHOD || op == XI_CALL_METHOD_DIRECT;
}

static bool is_field_access_site(uint16_t op) {
    return op == XI_LOAD_FIELD || op == XI_STORE_FIELD;
}

static void attach_method_ic(XiIcTable *table, const XiValue *v,
                             const XrICMethodTable *ic_methods) {
    if (!ic_methods)
        return;

    int ic_idx = (int) ((v->aux_int >> 1) & 0x7FFFFFFF);
    if (ic_idx < 0 || ic_idx >= ic_methods->count)
        return;

    XrICMethod *ic = &ic_methods->caches[ic_idx];
    XiIcKind kind = classify_method_ic(ic);
    if (kind == XI_IC_NONE)
        return;

    XiIcMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.value_id = v->id;
    meta.kind = kind;
    meta.total_count = ic->total_count;

    uint32_t n = ic->count;
    if (n > XI_IC_MAX_TARGETS)
        n = XI_IC_MAX_TARGETS;
    meta.ntargets = n;
    for (uint32_t i = 0; i < n; i++) {
        meta.targets[i].type_id =
            ic->entries[i].klass ? (uint32_t) (uintptr_t) ic->entries[i].klass : 0;
        meta.targets[i].hit_count = ic->entries[i].hit_count;
        meta.targets[i].field_id = 0;
    }

    table_add(table, &meta);
}

static void attach_field_ic(XiIcTable *table, const XiValue *v, const XrICFieldTable *ic_fields) {
    if (!ic_fields)
        return;

    int ic_idx = (int) (v->aux_int & 0xFFFF);
    if (ic_idx < 0 || ic_idx >= ic_fields->count)
        return;

    XrICField *ic = &ic_fields->caches[ic_idx];
    XiIcKind kind = classify_field_ic(ic);
    if (kind == XI_IC_NONE)
        return;

    XiIcMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.value_id = v->id;
    meta.kind = kind;
    meta.total_count = 0;

    uint32_t n = ic->entry_count;
    if (n > XI_IC_MAX_TARGETS)
        n = XI_IC_MAX_TARGETS;
    meta.ntargets = n;
    for (uint32_t i = 0; i < n; i++) {
        meta.targets[i].type_id =
            ic->entries[i].cls ? (uint32_t) (uintptr_t) ic->entries[i].cls : 0;
        meta.targets[i].hit_count = ic->entries[i].hit_count;
        meta.targets[i].field_id = (uint32_t) ic->entries[i].offset;
    }

    table_add(table, &meta);
}

XR_FUNC bool xi_ic_attach(XiFunc *f, struct XrICFieldTable *ic_fields,
                          struct XrICMethodTable *ic_methods) {
    XR_DCHECK(f != NULL, "xi_ic_attach: NULL func");

    if (f->ic_table) {
        xi_ic_table_free(f->ic_table);
        f->ic_table = NULL;
    }

    XiIcTable *table = table_new(8);
    if (!table)
        return false;

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            if (is_method_call_site(v->op))
                attach_method_ic(table, v, ic_methods);
            else if (is_field_access_site(v->op))
                attach_field_ic(table, v, ic_fields);
        }
    }

    f->ic_table = table;
    f->invariant_mask |= XI_INV_IC_ATTACHED;
    return true;
}

/* ========== Query ========== */

XR_FUNC const XiIcMeta *xi_ic_lookup(const XiFunc *f, uint32_t value_id) {
    if (!f || !f->ic_table)
        return NULL;
    const XiIcTable *t = f->ic_table;
    for (uint32_t i = 0; i < t->nentries; i++) {
        if (t->entries[i].value_id == value_id)
            return &t->entries[i];
    }
    return NULL;
}
