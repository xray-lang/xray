/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_ic.c - Per-coroutine inline-cache table management.
 *
 * KEY CONCEPT:
 *   Inline caches (field IC, method IC, Json shape IC) are kept on each
 *   XrVMContext rather than the shared, immutable XrProto. This file owns
 *   the bookkeeping: lazy allocation per (proto_id), deep-copy snapshots
 *   for JIT/AOT consumers, and full teardown on coroutine release.
 *
 * INVARIANTS:
 *   - ic_field_tables / ic_method_tables grow in lockstep; both arrays are
 *     always sized to ic_tables_capacity.
 *   - A NULL slot means "no IC recorded yet for this proto in this ctx".
 *   - Snapshots returned to JIT are independently owned and never alias
 *     live ctx storage; they are safe to read from a background thread
 *     after the ctx mutates further.
 */

#include "xvm_internal.h"
#include "xic_field_table.h"
#include "xic_method.h"
#include "xic_builtin.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xchunk.h"
#include <string.h>

/* ========== Internal: capacity management ========== */

static bool ic_tables_grow(XrVMContext *ctx, uint32_t needed) {
    XR_DCHECK(ctx != NULL, "ic_tables_grow: NULL ctx");
    XR_DCHECK(needed > ctx->ic_tables_capacity,
              "ic_tables_grow: needed must exceed current capacity");

    uint32_t new_cap = ctx->ic_tables_capacity ? ctx->ic_tables_capacity : 16u;
    while (new_cap < needed) {
        // Saturate at UINT32_MAX/2 to avoid overflow.
        if (new_cap > (UINT32_MAX / 2u)) {
            new_cap = needed;
            break;
        }
        new_cap *= 2u;
    }

    size_t fbytes = sizeof(struct XrICFieldTable *) * (size_t)new_cap;
    struct XrICFieldTable **new_field =
        (struct XrICFieldTable **)xr_realloc(ctx->ic_field_tables, fbytes);
    if (!new_field) return false;
    ctx->ic_field_tables = new_field;

    size_t mbytes = sizeof(struct XrICMethodTable *) * (size_t)new_cap;
    struct XrICMethodTable **new_method =
        (struct XrICMethodTable **)xr_realloc(ctx->ic_method_tables, mbytes);
    if (!new_method) {
        // Field array already grew; leave capacity at old value so the
        // tail of new_field stays unused. Other call sites tolerate
        // sparse capacity > old via the per-array NULL convention.
        return false;
    }
    ctx->ic_method_tables = new_method;

    size_t bbytes = sizeof(struct XrICBuiltinTable *) * (size_t)new_cap;
    struct XrICBuiltinTable **new_builtin =
        (struct XrICBuiltinTable **)xr_realloc(ctx->ic_builtin_tables, bbytes);
    if (!new_builtin) {
        // Field/method arrays already grew; leave capacity at old value
        // so their tails stay unused (per-array NULL convention).
        return false;
    }
    ctx->ic_builtin_tables = new_builtin;

    for (uint32_t i = ctx->ic_tables_capacity; i < new_cap; i++) {
        ctx->ic_field_tables[i] = NULL;
        ctx->ic_method_tables[i] = NULL;
        ctx->ic_builtin_tables[i] = NULL;
    }
    ctx->ic_tables_capacity = new_cap;
    return true;
}

/* ========== Lazy allocation (write-side) ========== */

XrICFieldTable *xr_vm_ctx_ensure_ic_fields(XrVMContext *ctx, XrProto *proto) {
    XR_DCHECK(ctx != NULL, "ensure_ic_fields: NULL ctx");
    XR_DCHECK(proto != NULL, "ensure_ic_fields: NULL proto");

    uint32_t pid = proto->proto_id;
    if (pid >= ctx->ic_tables_capacity) {
        if (!ic_tables_grow(ctx, pid + 1u)) return NULL;
    }

    XrICFieldTable *table = ctx->ic_field_tables[pid];
    if (table) return table;

    int cache_count = PROTO_CODE_COUNT(proto);
    table = xr_ic_field_table_new(cache_count);
    if (!table) return NULL;

    // Pre-allocate all IC slots so cache_index = pc - PROTO_CODE_BASE
    // is always a valid lookup key (matches pre-migration behaviour).
    for (int i = 0; i < cache_count; i++) {
        if (xr_ic_field_table_alloc(table) < 0) {
            xr_ic_field_table_free(table);
            return NULL;
        }
    }

    ctx->ic_field_tables[pid] = table;
    return table;
}

XrICMethodTable *xr_vm_ctx_ensure_ic_methods(XrVMContext *ctx, XrProto *proto) {
    XR_DCHECK(ctx != NULL, "ensure_ic_methods: NULL ctx");
    XR_DCHECK(proto != NULL, "ensure_ic_methods: NULL proto");

    uint32_t pid = proto->proto_id;
    if (pid >= ctx->ic_tables_capacity) {
        if (!ic_tables_grow(ctx, pid + 1u)) return NULL;
    }

    XrICMethodTable *table = ctx->ic_method_tables[pid];
    if (table) return table;

    int cache_count = PROTO_CODE_COUNT(proto);
    table = xr_ic_method_table_new(cache_count);
    if (!table) return NULL;

    // Method IC table relies on count == cache_count; xr_ic_method_table_new
    // already zeroes all entries, so we just publish the count.
    table->count = cache_count;

    ctx->ic_method_tables[pid] = table;
    return table;
}

XrICBuiltinTable *xr_vm_ctx_ensure_ic_builtin(XrVMContext *ctx, XrProto *proto) {
    XR_DCHECK(ctx != NULL, "ensure_ic_builtin: NULL ctx");
    XR_DCHECK(proto != NULL, "ensure_ic_builtin: NULL proto");

    uint32_t pid = proto->proto_id;
    if (pid >= ctx->ic_tables_capacity) {
        if (!ic_tables_grow(ctx, pid + 1u)) return NULL;
    }

    XrICBuiltinTable *table = ctx->ic_builtin_tables[pid];
    if (table) return table;

    /* Pre-size to PROTO_CODE_COUNT so cache_index = pc - PROTO_CODE_BASE
     * is always a valid lookup key (matches the field/method tables). */
    int cache_count = PROTO_CODE_COUNT(proto);
    table = xr_ic_builtin_table_new(cache_count);
    if (!table) return NULL;
    for (int i = 0; i < cache_count; i++) {
        if (xr_ic_builtin_table_alloc(table) < 0) {
            xr_ic_builtin_table_free(table);
            return NULL;
        }
    }

    ctx->ic_builtin_tables[pid] = table;
    return table;
}

/* ========== Read-side accessors ========== */

XrICFieldTable *xr_vm_ctx_get_ic_fields(const XrVMContext *ctx,
                                        const XrProto *proto) {
    if (!ctx || !proto) return NULL;
    uint32_t pid = proto->proto_id;
    if (pid >= ctx->ic_tables_capacity || !ctx->ic_field_tables) return NULL;
    return ctx->ic_field_tables[pid];
}

XrICMethodTable *xr_vm_ctx_get_ic_methods(const XrVMContext *ctx,
                                          const XrProto *proto) {
    if (!ctx || !proto) return NULL;
    uint32_t pid = proto->proto_id;
    if (pid >= ctx->ic_tables_capacity || !ctx->ic_method_tables) return NULL;
    return ctx->ic_method_tables[pid];
}

XrICBuiltinTable *xr_vm_ctx_get_ic_builtin(const XrVMContext *ctx,
                                            const XrProto *proto) {
    if (!ctx || !proto) return NULL;
    uint32_t pid = proto->proto_id;
    if (pid >= ctx->ic_tables_capacity || !ctx->ic_builtin_tables) return NULL;
    return ctx->ic_builtin_tables[pid];
}

/* ========== Snapshot API for JIT/AOT consumers ========== */

XrICFieldTable *xr_vm_ic_fields_snapshot(XrVMContext *ctx, XrProto *proto) {
    XrICFieldTable *src = xr_vm_ctx_get_ic_fields(ctx, proto);
    if (!src || src->count == 0) return NULL;

    XrICFieldTable *dst = xr_ic_field_table_new(src->count);
    if (!dst) return NULL;
    for (int i = 0; i < src->count; i++) {
        if (xr_ic_field_table_alloc(dst) < 0) {
            xr_ic_field_table_free(dst);
            return NULL;
        }
    }
    XR_DCHECK(dst->count == src->count, "snapshot: count mismatch");
    if (src->count > 0) {
        memcpy(dst->caches, src->caches, sizeof(XrICField) * (size_t)src->count);
    }
    return dst;
}

XrICMethodTable *xr_vm_ic_methods_snapshot(XrVMContext *ctx, XrProto *proto) {
    XrICMethodTable *src = xr_vm_ctx_get_ic_methods(ctx, proto);
    if (!src || src->count == 0) return NULL;

    XrICMethodTable *dst = xr_ic_method_table_new(src->count);
    if (!dst) return NULL;
    dst->count = src->count;

    if (src->count > 0) {
        memcpy(dst->caches, src->caches,
               sizeof(XrICMethod) * (size_t)src->count);

        // Mega caches are heap-allocated; deep-copy each one so the
        // snapshot is independent of the live ctx.
        for (int i = 0; i < src->count; i++) {
            XrMegaCache *mc_src = src->caches[i].mega_cache;
            if (!mc_src) {
                dst->caches[i].mega_cache = NULL;
                continue;
            }
            XrMegaCache *mc_dst = (XrMegaCache *)xr_malloc(sizeof(XrMegaCache));
            if (!mc_dst) {
                // Best-effort: drop the mega cache on this entry; the JIT
                // consumer can fall back to poly entries / class lookup.
                dst->caches[i].mega_cache = NULL;
                continue;
            }
            *mc_dst = *mc_src;
            dst->caches[i].mega_cache = mc_dst;
        }
    }
    return dst;
}

/* ========== Teardown ========== */

void xr_vm_ctx_free_ic_tables(XrVMContext *ctx) {
    if (!ctx) return;

    if (ctx->ic_field_tables) {
        for (uint32_t i = 0; i < ctx->ic_tables_capacity; i++) {
            if (ctx->ic_field_tables[i]) {
                xr_ic_field_table_free(ctx->ic_field_tables[i]);
                ctx->ic_field_tables[i] = NULL;
            }
        }
        xr_free(ctx->ic_field_tables);
        ctx->ic_field_tables = NULL;
    }
    if (ctx->ic_method_tables) {
        for (uint32_t i = 0; i < ctx->ic_tables_capacity; i++) {
            if (ctx->ic_method_tables[i]) {
                xr_ic_method_table_free(ctx->ic_method_tables[i]);
                ctx->ic_method_tables[i] = NULL;
            }
        }
        xr_free(ctx->ic_method_tables);
        ctx->ic_method_tables = NULL;
    }
    if (ctx->ic_builtin_tables) {
        for (uint32_t i = 0; i < ctx->ic_tables_capacity; i++) {
            if (ctx->ic_builtin_tables[i]) {
                xr_ic_builtin_table_free(ctx->ic_builtin_tables[i]);
                ctx->ic_builtin_tables[i] = NULL;
            }
        }
        xr_free(ctx->ic_builtin_tables);
        ctx->ic_builtin_tables = NULL;
    }
    ctx->ic_tables_capacity = 0;
}
