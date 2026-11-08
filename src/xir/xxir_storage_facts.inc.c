/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_facts.inc.c - Idempotent numeric storage relations
 *
 * KEY CONCEPT:
 *   Reprocessing an application adds only new facts. Exposure closure cannot
 *   manufacture a contents change by appending an existing dependency again.
 */

static uint32_t storage_relation_hash(XrXirValueFlow edge) {
    uint32_t fields[] = {edge.source, edge.target};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool storage_relation_index_grow(XirStorageFlow *flow, uint32_t function, XirStorageFlowBank bank) {
    StorageFunction *row = &flow->rows[function];
    XirStorageFlowFunction view = flow->view.functions[function];
    uint32_t **old = bank == XIR_STORAGE_EXPOSURE ? &row->exposure_index : &row->flow_index;
    uint32_t *size = bank == XIR_STORAGE_EXPOSURE ? &row->exposure_index_capacity : &row->flow_index_capacity;
    uint64_t next = *size ? (uint64_t)*size * 2 : 16;
    if (next > UINT32_MAX) { flow->status = XR_XIR_OVERFLOW; return false; }
    uint32_t capacity = (uint32_t)next;
    uint32_t *index = xir_compile_calloc(&flow->memory, capacity, sizeof(*index), &flow->status);
    if (!index) return false;
    const XrXirValueFlow *edges = bank == XIR_STORAGE_EXPOSURE ? view.exposure_flows : view.flows;
    uint32_t count = bank == XIR_STORAGE_EXPOSURE ? view.exposure_flow_count : view.flow_count;
    for (uint32_t e = 0; e < count && storage_step(flow); ++e) {
        uint32_t slot = storage_relation_hash(edges[e]) & (capacity - 1);
        while (index[slot] && storage_step(flow)) slot = (slot + 1) & (capacity - 1);
        index[slot] = e + 1;
    }
    if (flow->status != XR_XIR_OK) { xr_compile_resources_free(index); return false; }
    xr_compile_resources_free(*old); *old = index; *size = capacity;
    return true;
}

static bool storage_relation_bank(XirStorageFlow *flow, uint32_t function, uint32_t source,
    uint32_t target, XirStorageFlowBank bank) {
    if (flow->status != XR_XIR_OK) return false;
    XirStorageFlowFunction *row = (XirStorageFlowFunction *)&flow->view.functions[function];
    StorageFunction *work = &flow->rows[function];
    if (source >= row->value_count || target >= row->value_count) {
        flow->status = XR_XIR_BAD_STRUCTURE; return false;
    }
    const XrXirValueFlow **edges = bank == XIR_STORAGE_EXPOSURE ? &row->exposure_flows : &row->flows;
    uint32_t *count = bank == XIR_STORAGE_EXPOSURE ? &row->exposure_flow_count : &row->flow_count;
    uint32_t *capacity = bank == XIR_STORAGE_EXPOSURE ? &work->exposure_capacity : &work->flow_capacity;
    uint32_t **index = bank == XIR_STORAGE_EXPOSURE ? &work->exposure_index : &work->flow_index;
    uint32_t *index_capacity = bank == XIR_STORAGE_EXPOSURE ? &work->exposure_index_capacity : &work->flow_index_capacity;
    if (!*index_capacity || *count >= *index_capacity / 2)
        if (!storage_relation_index_grow(flow, function, bank)) return false;
    uint32_t slot = storage_relation_hash((XrXirValueFlow){source, target}) & (*index_capacity - 1);
    while ((*index)[slot] && storage_step(flow)) {
        XrXirValueFlow previous = (*edges)[(*index)[slot] - 1];
        if (previous.source == source && previous.target == target) return true;
        slot = (slot + 1) & (*index_capacity - 1);
    }
    if (flow->status != XR_XIR_OK) return false;
    void *values = (void *)*edges;
    if (!storage_reserve(flow, &values, *count, capacity, sizeof(**edges))) return false;
    *edges = values;
    ((XrXirValueFlow *)values)[*count] = (XrXirValueFlow){source, target};
    (*index)[slot] = ++*count;
    ++flow->revision;
    return true;
}

static bool storage_relation(XirStorageFlow *flow, uint32_t function, uint32_t source, uint32_t target) {
    return storage_relation_bank(flow, function, source, target, XIR_STORAGE_CONTENTS);
}

static bool storage_state_relation(XirStorageFlow *flow, uint32_t function, StorageState state, uint32_t source) {
    return storage_relation_bank(flow, function, source, state.value, storage_bank(state));
}

static uint32_t storage_seed_hash(XirStorageExposureSeed seed) {
    uint32_t fields[] = {seed.function, seed.value, seed.application};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool storage_seed_index_grow(XirStorageFlow *flow) {
    uint64_t next = flow->exposure_seed_index_capacity ? (uint64_t)flow->exposure_seed_index_capacity * 2 : 16;
    if (next > UINT32_MAX) { flow->status = XR_XIR_OVERFLOW; return false; }
    uint32_t capacity = (uint32_t)next;
    uint32_t *index = xir_compile_calloc(&flow->memory, capacity, sizeof(*index), &flow->status);
    if (!index) return false;
    for (uint32_t e = 0; e < flow->view.exposure_seed_count && storage_step(flow); ++e) {
        uint32_t slot = storage_seed_hash(flow->view.exposure_seeds[e]) & (capacity - 1);
        while (index[slot] && storage_step(flow)) slot = (slot + 1) & (capacity - 1);
        index[slot] = e + 1;
    }
    if (flow->status != XR_XIR_OK) { xr_compile_resources_free(index); return false; }
    xr_compile_resources_free(flow->exposure_seed_index);
    flow->exposure_seed_index = index; flow->exposure_seed_index_capacity = capacity;
    return true;
}

static bool storage_exposure_seed(XirStorageFlow *flow, uint32_t function, uint32_t value, uint32_t application) {
    if (flow->status != XR_XIR_OK) return false;
    if (!flow->exposure_seed_index_capacity || flow->view.exposure_seed_count >= flow->exposure_seed_index_capacity / 2)
        if (!storage_seed_index_grow(flow)) return false;
    XirStorageExposureSeed seed = {function, value, application};
    uint32_t slot = storage_seed_hash(seed) & (flow->exposure_seed_index_capacity - 1);
    while (flow->exposure_seed_index[slot] && storage_step(flow)) {
        XirStorageExposureSeed previous = flow->view.exposure_seeds[flow->exposure_seed_index[slot] - 1];
        if (previous.function == function && previous.value == value && previous.application == application) return true;
        slot = (slot + 1) & (flow->exposure_seed_index_capacity - 1);
    }
    if (flow->status != XR_XIR_OK) return false;
    void *values = (void *)flow->view.exposure_seeds;
    if (!storage_reserve(flow, &values, flow->view.exposure_seed_count, &flow->exposure_seed_capacity,
        sizeof(*flow->view.exposure_seeds))) return false;
    flow->view.exposure_seeds = values;
    ((XirStorageExposureSeed *)values)[flow->view.exposure_seed_count] = seed;
    flow->exposure_seed_index[slot] = ++flow->view.exposure_seed_count;
    ++flow->revision;
    return true;
}

static uint32_t storage_unknown_hash(XirStorageUnknown unknown) {
    uint32_t fields[] = {unknown.function, unknown.value, unknown.type, unknown.application};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool storage_unknown_index_grow(XirStorageFlow *flow) {
    uint64_t next = flow->unknown_index_capacity ? (uint64_t)flow->unknown_index_capacity * 2 : 16;
    if (next > UINT32_MAX) { flow->status = XR_XIR_OVERFLOW; return false; }
    uint32_t capacity = (uint32_t)next;
    uint32_t *index = xir_compile_calloc(&flow->memory, capacity, sizeof(*index), &flow->status);
    if (!index) return false;
    for (uint32_t e = 0; e < flow->view.unknown_count && storage_step(flow); ++e) {
        uint32_t slot = storage_unknown_hash(flow->view.unknowns[e]) & (capacity - 1);
        while (index[slot] && storage_step(flow)) slot = (slot + 1) & (capacity - 1);
        index[slot] = e + 1;
    }
    if (flow->status != XR_XIR_OK) { xr_compile_resources_free(index); return false; }
    xr_compile_resources_free(flow->unknown_index); flow->unknown_index = index; flow->unknown_index_capacity = capacity;
    return true;
}

static bool storage_unknown(XirStorageFlow *flow, uint32_t function, uint32_t value, XrXirType type) {
    if (flow->status != XR_XIR_OK) return false;
    if (!flow->unknown_index_capacity || flow->view.unknown_count >= flow->unknown_index_capacity / 2)
        if (!storage_unknown_index_grow(flow)) return false;
    XirStorageUnknown unknown = {function, value, type, UINT32_MAX};
    uint32_t slot = storage_unknown_hash(unknown) & (flow->unknown_index_capacity - 1);
    while (flow->unknown_index[slot] && storage_step(flow)) {
        XirStorageUnknown previous = flow->view.unknowns[flow->unknown_index[slot] - 1];
        if (previous.function == function && previous.value == value && previous.type == type &&
            previous.application == unknown.application) return true;
        slot = (slot + 1) & (flow->unknown_index_capacity - 1);
    }
    if (flow->status != XR_XIR_OK) return false;
    void *values = (void *)flow->view.unknowns;
    if (!storage_reserve(flow, &values, flow->view.unknown_count, &flow->unknown_capacity,
        sizeof(*flow->view.unknowns))) return false;
    flow->view.unknowns = values;
    ((XirStorageUnknown *)values)[flow->view.unknown_count] = unknown;
    flow->unknown_index[slot] = ++flow->view.unknown_count;
    ++flow->revision;
    return true;
}

static uint32_t storage_cross_hash(XirStorageCrossFlow edge) {
    uint32_t fields[] = {edge.source_function, edge.source, edge.target_function, edge.target,
        edge.application, (uint32_t)edge.bank};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool storage_same_cross(XirStorageCrossFlow a, XirStorageCrossFlow b) {
    return a.source_function == b.source_function && a.source == b.source &&
        a.target_function == b.target_function && a.target == b.target && a.application == b.application && a.bank == b.bank;
}

static bool storage_cross_index_grow(XirStorageFlow *flow) {
    uint64_t next = flow->cross_index_capacity ? (uint64_t)flow->cross_index_capacity * 2 : 16;
    if (next > UINT32_MAX) { flow->status = XR_XIR_OVERFLOW; return false; }
    uint32_t capacity = (uint32_t)next;
    uint32_t *index = xir_compile_calloc(&flow->memory, capacity, sizeof(*index), &flow->status);
    if (!index) return false;
    for (uint32_t e = 0; e < flow->view.crossflow_count && storage_step(flow); ++e) {
        uint32_t slot = storage_cross_hash(flow->view.crossflows[e]) & (capacity - 1);
        while (index[slot] && storage_step(flow)) slot = (slot + 1) & (capacity - 1);
        index[slot] = e + 1;
    }
    if (flow->status != XR_XIR_OK) { xr_compile_resources_free(index); return false; }
    xr_compile_resources_free(flow->cross_index); flow->cross_index = index; flow->cross_index_capacity = capacity;
    return true;
}

static bool storage_cross(XirStorageFlow *flow, XirStorageCrossFlow edge) {
    if (flow->status != XR_XIR_OK) return false;
    if (edge.source_function >= flow->view.function_count || edge.target_function >= flow->view.function_count ||
        edge.source >= flow->view.functions[edge.source_function].value_count ||
        edge.target >= flow->view.functions[edge.target_function].value_count) {
        flow->status = XR_XIR_BAD_STRUCTURE; return false;
    }
    if (!flow->cross_index_capacity || flow->view.crossflow_count >= flow->cross_index_capacity / 2)
        if (!storage_cross_index_grow(flow)) return false;
    uint32_t slot = storage_cross_hash(edge) & (flow->cross_index_capacity - 1);
    while (flow->cross_index[slot] && storage_step(flow)) {
        if (storage_same_cross(flow->view.crossflows[flow->cross_index[slot] - 1], edge)) return true;
        slot = (slot + 1) & (flow->cross_index_capacity - 1);
    }
    if (flow->status != XR_XIR_OK) return false;
    void *values = (void *)flow->view.crossflows;
    if (!storage_reserve(flow, &values, flow->view.crossflow_count, &flow->crossflow_capacity,
        sizeof(*flow->view.crossflows))) return false;
    flow->view.crossflows = values;
    ((XirStorageCrossFlow *)values)[flow->view.crossflow_count] = edge;
    flow->cross_index[slot] = ++flow->view.crossflow_count;
    ++flow->revision;
    return true;
}
