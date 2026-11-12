/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_call_panic.inc.c - Qualified plain-Cell writer panic paths
 */
#include "xxir_storage_panic_cache.inc.c"

typedef struct StoragePanicFrame {
    XirExecutionSite site;
    uint32_t parent;
} StoragePanicFrame;

/* Follow the actual carrier before identity collapse. Projection and unknown
 * PHI barriers remain visible even when their base is an owned plain Cell. */
static bool storage_panic_plain_cell(XirStorageFlow *flow, const StoragePanicFrame *frames,
    uint32_t frame, uint32_t function, uint32_t value, uint32_t instance,
    StoragePanicIdentity *identity) {
    uint32_t environment = XIR_EXECUTION_CURRENT_ENVIRONMENT;
    for (;;) {
        const XrXirFunction *body = flow->rows[function].body;
        if (value >= body->parameter_count) {
            if (value >= flow->view.functions[function].physical_values) return false;
            StorageOperation op = flow->rows[function].operations[value - body->parameter_count];
            if (op.op == XR_XIR_CELL_NEW || op.op == XR_XIR_CELL_NEW_TAKE) {
                *identity = (StoragePanicIdentity){function, value, instance, environment};
                return true;
            }
            if (!storage_copy(op.op)) return false;
            value = op.source;
            continue;
        }
        if (frame == UINT32_MAX || frames[frame].site.declaration != function ||
            frames[frame].site.target != instance) return false;
        StoragePanicFrame current = frames[frame];
        if (value >= current.site.argument_count) return false;
        XirExecutionArgument actual = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, &current.site, value, &actual);
        if (status != XR_XIR_OK) {
            if (status != XR_XIR_UNRESOLVED) flow->status = status;
            return false;
        }
        if (!actual.cell || actual.function != current.site.function ||
            actual.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT) return false;
        function = actual.function;
        value = actual.value;
        instance = current.site.caller_instance;
        environment = actual.environment;
        frame = current.parent;
    }
}

static bool storage_panic_frame_key(XirStorageFlow *flow, const StoragePanicFrame *frames,
    uint32_t cursor, uint32_t application, StoragePanicKey *key, StoragePanicIdentity **identities) {
    StoragePanicFrame frame = frames[cursor];
    const XrXirFunction *body = flow->rows[frame.site.declaration].body;
    uint32_t cells = 0;
    for (uint32_t p = 0; p < body->parameter_count; ++p)
        cells += xr_xir_type_is_cell(flow->module->types, body->parameters[p]);
    *identities = cells ? xir_compile_alloc(&flow->memory, (size_t)cells * sizeof(**identities), &flow->status) : NULL;
    if (flow->status != XR_XIR_OK) return false;
    uint32_t written = 0;
    for (uint32_t p = 0; p < body->parameter_count; ++p) {
        if (!xr_xir_type_is_cell(flow->module->types, body->parameters[p])) continue;
        if (!storage_panic_plain_cell(flow, frames, cursor, frame.site.declaration, p,
                frame.site.target, &(*identities)[written++])) return false;
    }
    *key = (StoragePanicKey){application, frame.site.target, 0, cells};
    return true;
}

static bool storage_panic_frame_add(XirStorageFlow *flow, StoragePanicCache *query,
    StoragePanicFrame **frames, uint32_t *capacity, StoragePanicFrame frame, uint32_t application) {
    uint32_t cursor = query->count;
    if (!storage_reserve(flow, (void **)frames, cursor, capacity, sizeof(**frames))) return false;
    (*frames)[cursor] = frame;
    if (frame.site.unresolved || frame.site.native_callable || frame.site.declaration >= flow->view.function_count ||
        flow->rows[frame.site.declaration].panic_independent) return false;
    StoragePanicIdentity *identities = NULL;
    StoragePanicKey key = {0};
    bool closed = storage_panic_frame_key(flow, *frames, cursor, application, &key, &identities);
    if (closed && storage_panic_cache_find(&flow->panic_cache, key, identities) == UINT32_MAX &&
        storage_panic_cache_find(query, key, identities) == UINT32_MAX)
        closed = storage_panic_cache_add(flow, query, key, identities);
    xr_compile_resources_free(identities);
    return closed;
}

static bool storage_call_no_panic(XirStorageFlow *flow, const XirExecutionSite *site, uint32_t application) {
    if (site->unresolved || site->native_callable || site->declaration >= flow->view.function_count ||
        flow->rows[site->declaration].panic_independent) return false;
    StoragePanicCache *cache = &flow->panic_cache;
    if (!cache->ready || cache->generation != flow->generation) {
        cache->count = cache->identity_count = 0;
        if (cache->index_capacity) memset(cache->index, 0, (size_t)cache->index_capacity * sizeof(*cache->index));
        cache->generation = flow->generation;
        cache->ready = true;
    }
    StoragePanicFrame *frames = NULL;
    StoragePanicCache query = {0};
    uint32_t capacity = 0;
    bool closed = false;
    if (!storage_panic_frame_add(flow, &query, &frames, &capacity,
            (StoragePanicFrame){*site, UINT32_MAX}, application)) goto done;
    for (uint32_t cursor = 0; cursor < query.count && storage_step(flow); ++cursor) {
        StoragePanicFrame frame = frames[cursor];
        StorageFunction *row = &flow->rows[frame.site.declaration];
        for (uint32_t w = 0; w < row->write_count && storage_step(flow); ++w) {
            StorageWrite write = row->writes[w];
            if (!storage_write(row->operations[write.point].op)) continue;
            StoragePanicIdentity identity;
            if (!storage_panic_plain_cell(flow, frames, cursor, frame.site.declaration,
                    write.handle, frame.site.target, &identity)) goto done;
        }
        for (uint32_t c = 0; c < row->call_count && storage_step(flow); ++c) {
            uint32_t point = row->calls[c];
            if (storage_cleanup_exit(row->operations[point].op)) continue;
            if (row->operations[point].op != XR_XIR_CALL) goto done;
            bool found = false;
            for (uint32_t s = row->site_heads[point]; s != UINT32_MAX; s = flow->sites[s].next) {
                XirExecutionSite child = flow->sites[s].site;
                if (child.caller_instance != frame.site.target || storage_environment_site(&child)) continue;
                for (uint32_t p = cursor; p != UINT32_MAX; p = frames[p].parent)
                    if (frames[p].site.target == child.target) goto done;
                if (!storage_panic_frame_add(flow, &query, &frames, &capacity,
                        (StoragePanicFrame){child, cursor}, application)) goto done;
                found = true;
            }
            if (!found) goto done;
        }
    }
    closed = flow->status == XR_XIR_OK;
    for (uint32_t q = 0; closed && q < query.count; ++q) {
        StoragePanicKey key = query.keys[q];
        closed = storage_panic_cache_add(flow, cache, key, key.count ? query.identities + key.first : NULL);
    }
done:
    storage_panic_cache_clear(&query);
    xr_compile_resources_free(frames);
    return closed;
}
