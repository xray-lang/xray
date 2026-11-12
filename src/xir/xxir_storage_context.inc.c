/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_context.inc.c - Owned relative application environments
 */
static uint32_t storage_context_hash(uint32_t source, uint32_t parent) {
    uint32_t fields[] = {source, parent};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool storage_context_index(XirStorageFlow *flow) {
    uint64_t next = flow->context_index_capacity ? (uint64_t)flow->context_index_capacity * 2 : 16;
    if (next > UINT32_MAX || next > SIZE_MAX / sizeof(*flow->context_index)) {
        flow->status = XR_XIR_OVERFLOW;
        return false;
    }
    uint32_t *index = xir_compile_calloc(&flow->memory, (size_t)next, sizeof(*index), &flow->status);
    if (!index) return false;
    for (uint32_t a = 0; a < flow->view.application_count; ++a) {
        XirStorageApplication app = flow->view.applications[a];
        if (!app.context_only) continue;
        uint32_t slot = storage_context_hash(app.source_application, app.environment) & ((uint32_t)next - 1);
        while (index[slot]) slot = (slot + 1) & ((uint32_t)next - 1);
        index[slot] = a + 1;
    }
    xr_compile_resources_free(flow->context_index);
    flow->context_index = index;
    flow->context_index_capacity = (uint32_t)next;
    return true;
}

static void storage_context_refresh(XirStorageFlow *flow) {
    for (uint32_t a = 0; a < flow->view.application_count; ++a) {
        XirStorageApplication app = flow->view.applications[a];
        if (!app.context_only) continue;
        XirStorageApplication source = flow->view.applications[app.source_application];
        XirStorageApplication *target = (XirStorageApplication *)&flow->view.applications[a];
        target->first = source.first;
        target->count = source.count;
    }
}

XR_FUNC XrXirStatus xir_storage_flow_compose_environment(XirStorageFlow *flow,
    uint32_t application, uint32_t parent, uint32_t *output) {
    if (!flow || !output || application >= flow->view.application_count || parent >= flow->view.application_count)
        return XR_XIR_BAD_STRUCTURE;
    XirStorageApplication source = flow->view.applications[application];
    if (source.context_only || source.kind == XIR_EXECUTION_CALLBACK_ENTRY ||
        source.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT ||
        flow->view.applications[parent].target != source.caller_instance) return XR_XIR_UNRESOLVED;
    uint32_t ancestor = parent, steps = 0;
    while (ancestor < XIR_EXECUTION_UNKNOWN_ENVIRONMENT) {
        if (ancestor >= flow->view.application_count || ++steps > flow->view.application_count) return XR_XIR_UNRESOLVED;
        XirStorageApplication frame = flow->view.applications[ancestor];
        if ((frame.context_only ? frame.source_application : ancestor) == application) return XR_XIR_UNRESOLVED;
        ancestor = frame.environment;
    }
    if (flow->context_index_capacity) {
        uint32_t slot = storage_context_hash(application, parent) & (flow->context_index_capacity - 1);
        while (flow->context_index[slot]) {
            uint32_t id = flow->context_index[slot] - 1;
            XirStorageApplication old = flow->view.applications[id];
            if (old.source_application == application && old.environment == parent) {*output = id; return XR_XIR_OK;}
            slot = (slot + 1) & (flow->context_index_capacity - 1);
        }
    } else if (flow->sealed) {
        for (uint32_t a = 0; a < flow->view.application_count; ++a) {
            XirStorageApplication old = flow->view.applications[a];
            if (old.context_only && old.source_application == application && old.environment == parent) {
                *output = a; return XR_XIR_OK;
            }
        }
    }
    if (flow->sealed) return XR_XIR_UNRESOLVED;
    if (!flow->context_index_capacity || flow->contexts >= flow->context_index_capacity / 2)
        if (!storage_context_index(flow)) return flow->status;
    if (flow->view.application_count >= XIR_EXECUTION_UNKNOWN_ENVIRONMENT) return flow->status = XR_XIR_OVERFLOW;
    void *values = (void *)flow->view.applications;
    if (!storage_reserve(flow, &values, flow->view.application_count,
            &flow->application_capacity, sizeof(*flow->view.applications))) return flow->status;
    flow->view.applications = values;
    source.environment = parent;
    source.source_application = application;
    source.context_only = true;
    uint32_t id = flow->view.application_count++;
    ((XirStorageApplication *)values)[id] = source;
    uint32_t slot = storage_context_hash(application, parent) & (flow->context_index_capacity - 1);
    while (flow->context_index[slot]) slot = (slot + 1) & (flow->context_index_capacity - 1);
    flow->context_index[slot] = id + 1;
    ++flow->contexts;
    if (++flow->generation == 0) return flow->status = XR_XIR_OVERFLOW;
    *output = id;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_storage_flow_environment(const XirStorageFlow *flow,
    uint32_t application, uint32_t parent, uint32_t *output) {
    if (!flow || !output) return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    const XirStorageFlowView *view = &flow->view;
    if (application >= view->application_count || parent >= view->application_count) return XR_XIR_BAD_STRUCTURE;
    XirStorageApplication source = view->applications[application];
    if (source.context_only || source.kind == XIR_EXECUTION_CALLBACK_ENTRY ||
        source.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT ||
        view->applications[parent].target != source.caller_instance) return XR_XIR_UNRESOLVED;
    if (flow->context_index_capacity) {
        uint32_t slot = storage_context_hash(application,parent) & (flow->context_index_capacity - 1);
        while (flow->context_index[slot]) {
            uint32_t id = flow->context_index[slot] - 1;
            XirStorageApplication context = view->applications[id];
            if (context.source_application == application && context.environment == parent) {
                *output = id;
                return XR_XIR_OK;
            }
            slot = (slot + 1) & (flow->context_index_capacity - 1);
        }
        return XR_XIR_UNRESOLVED;
    }
    for (uint32_t a = 0; a < view->application_count; ++a) {
        XirStorageApplication context = view->applications[a];
        if (context.context_only && context.source_application == application && context.environment == parent) {
            *output = a;
            return XR_XIR_OK;
        }
    }
    return XR_XIR_UNRESOLVED;
}
