/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_capture.inc.c - Point observations of contextual environments
 *
 * KEY CONCEPT:
 *   Execution owns callable origins. Storage follows its capture references
 *   and observes Cell contents at the use, separately from Cell identity.
 */

XR_FUNC XrXirStatus xir_storage_flow_reference(XirStorageFlow *flow, const XirStorageObservation *observation,
    uint32_t *contents, uint32_t *exposure) {
    if (!observation || (!contents && !exposure) || !storage_valid_point(flow, observation->function,
        observation->instruction, observation->point)) return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    flow->request_remainder = 0;
    XirStorageReference reference;
    XrXirStatus status = storage_current_reference(flow, observation->function, observation->reference, &reference);
    if (status != XR_XIR_OK) return status;
    uint32_t read = UINT32_MAX, published = UINT32_MAX;
    if (contents) status = xir_storage_flow_contents_at(flow, observation->function,
        observation->instruction, reference.value, observation->point, &read);
    if (status == XR_XIR_OK && exposure) status = xir_storage_flow_exposure_at(flow, observation->function,
        observation->instruction, reference.value, observation->point, &published);
    if (status == XR_XIR_OK) {
        if (contents) *contents = read;
        if (exposure) *exposure = published;
    }
    return status;
}

XR_FUNC XrXirStatus xir_storage_flow_expose_reference(XirStorageFlow *flow,
    const XirStorageObservation *observation) {
    if (!observation || !storage_valid_point(flow, observation->function, observation->instruction,
        observation->point)) return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    flow->request_remainder = 0;
    XirStorageReference reference;
    XrXirStatus status = storage_current_reference(flow, observation->function, observation->reference, &reference);
    return status == XR_XIR_OK ? xir_storage_flow_expose(flow, observation->function,
        observation->instruction, reference.value, observation->point) : status;
}

typedef enum StorageCarrierKind {
    STORAGE_CARRIER_FN, STORAGE_CARRIER_CELL, STORAGE_CARRIER_FORMAL, STORAGE_CARRIER_STORABLE
} StorageCarrierKind;
typedef struct StorageCaptureItem {
    XirStorageReference reference;
    StorageCarrierKind kind;
    uint32_t proxy;
} StorageCaptureItem;
typedef struct StorageCaptureWalk {
    XirStorageFlow *flow;
    StorageCaptureItem *items;
    uint32_t *index;
    uint32_t count, capacity, index_capacity, remainder;
    XirStorageReference current_proxy;
    bool complete;
} StorageCaptureWalk;

static uint32_t storage_capture_hash(StorageCaptureItem item) {
    uint32_t fields[] = {item.reference.function, item.reference.value, item.reference.environment, (uint32_t)item.kind,
        item.kind == STORAGE_CARRIER_FORMAL ? item.proxy : 0};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool storage_capture_push(StorageCaptureWalk *walk, StorageCaptureItem item) {
    XirStorageFlow *flow = walk->flow;
    if (!walk->index_capacity || walk->count >= walk->index_capacity / 2) {
        uint64_t next = walk->index_capacity ? (uint64_t)walk->index_capacity * 2 : 16;
        if (next > UINT32_MAX) { flow->status = XR_XIR_OVERFLOW; return false; }
        uint32_t capacity = (uint32_t)next;
        uint32_t *index = xir_compile_calloc(&flow->memory, capacity, sizeof(*index), &flow->status);
        if (!index) return false;
        for (uint32_t n = 0; n < walk->count && storage_step(flow); ++n) {
            uint32_t slot = storage_capture_hash(walk->items[n]) & (capacity - 1);
            while (index[slot]) slot = (slot + 1) & (capacity - 1);
            index[slot] = n + 1;
        }
        if (flow->status != XR_XIR_OK) { xr_compile_resources_free(index); return false; }
        xr_compile_resources_free(walk->index); walk->index = index; walk->index_capacity = capacity;
    }
    uint32_t slot = storage_capture_hash(item) & (walk->index_capacity - 1);
    while (walk->index[slot]) {
        StorageCaptureItem found = walk->items[walk->index[slot] - 1];
        if (found.kind == item.kind && (item.kind != STORAGE_CARRIER_FORMAL || found.proxy == item.proxy) &&
            found.reference.function == item.reference.function &&
            found.reference.value == item.reference.value && found.reference.environment == item.reference.environment) return true;
        slot = (slot + 1) & (walk->index_capacity - 1);
    }
    if (!storage_reserve(flow, (void **)&walk->items, walk->count, &walk->capacity, sizeof(*walk->items))) return false;
    walk->items[walk->count] = item; walk->index[slot] = ++walk->count;
    return true;
}

static XrXirStatus storage_capture_arguments(void *opaque, const XirExecutionSite *capture) {
    StorageCaptureWalk *walk = opaque;
    XirStorageFlow *flow = walk->flow;
    if (capture->kind == XIR_EXECUTION_STORABLE) {
        if (capture->argument_count || capture->receiver_function >= flow->view.function_count ||
            capture->receiver_value >= flow->view.functions[capture->receiver_function].physical_values ||
            !xr_xir_type_is_callable(xir_execution_types(flow->execution), capture->signature)) return XR_XIR_BAD_STRUCTURE;
        if (!storage_capture_push(walk, (StorageCaptureItem){
            .reference={capture->receiver_function, capture->receiver_value, capture->environment},
            .kind=STORAGE_CARRIER_STORABLE})) return flow->status;
        return XR_XIR_OK;
    }
    if (capture->kind == XIR_EXECUTION_FORMAL) {
        XirExecutionArgument formal = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, capture, 0, &formal);
        if (status != XR_XIR_OK) return status;
        if (!storage_capture_push(walk, (StorageCaptureItem){
            .reference={formal.function, formal.value, formal.environment},
            .kind=STORAGE_CARRIER_FORMAL, .proxy=walk->current_proxy.value})) return flow->status;
        return XR_XIR_OK;
    }
    for (uint32_t p = 0; p < capture->argument_count && storage_step(flow); ++p) {
        XirExecutionArgument actual = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, capture, p, &actual);
        if (status == XR_XIR_UNRESOLVED) {
            walk->remainder |= XIR_EXECUTION_REMAINDER_STORAGE;
            walk->complete = false;
            continue;
        }
        if (status != XR_XIR_OK) return status;
        if (!actual.cell) {
            const XrXirTypes *types = xir_execution_types(flow->execution);
            if (!xr_xir_type_is_callable(types, actual.type) || xr_xir_type_is_c_callable(types, actual.type)) continue;
        }
        if (!storage_capture_push(walk, (StorageCaptureItem){.reference={actual.function, actual.value, actual.environment},
            .kind=actual.cell ? STORAGE_CARRIER_CELL : STORAGE_CARRIER_FN}))
            return flow->status;
    }
    return flow->status;
}

static XrXirStatus storage_capture_collect(StorageCaptureWalk *walk, const XirStorageValuePoint *point) {
    XirStorageFlow *flow = walk->flow;
    if (!storage_capture_push(walk, (StorageCaptureItem){.reference=point->reference, .kind=STORAGE_CARRIER_FN})) return flow->status;
    for (uint32_t n = 0; n < walk->count && storage_step(flow); ++n) {
        StorageCaptureItem item = walk->items[n];
        if (item.kind == STORAGE_CARRIER_FORMAL || item.kind == STORAGE_CARRIER_STORABLE) continue;
        if (item.kind == STORAGE_CARRIER_CELL) {
            XirStorageReference local;
            XrXirStatus status = storage_current_reference(flow, point->function, item.reference, &local);
            if (status != XR_XIR_OK) return status;
            XrXirType payload = storage_payload(flow, local.function, local.value);
            if (!xr_xir_type_is_callable(flow->module->types, payload) ||
                xr_xir_type_is_c_callable(flow->module->types, payload)) continue;
            uint32_t contents;
            status = xir_storage_flow_contents_at(flow, point->function, point->instruction,
                local.value, point->point, &contents);
            if (status != XR_XIR_OK) return status;
            if (!storage_capture_push(walk, (StorageCaptureItem){
                .reference={point->function, contents, XIR_EXECUTION_CURRENT_ENVIRONMENT}, .kind=STORAGE_CARRIER_FN})) return flow->status;
            continue;
        }
        bool complete = false;
        uint32_t remainder = 0;
        XirStorageValuePoint use = *point;
        use.reference = item.reference;
        XirStorageReference proxy;
        XrXirStatus status = xir_storage_flow_point_value(flow, &use, &proxy);
        if (status != XR_XIR_OK) return status;
        walk->current_proxy = proxy;
        status = xir_execution_visit_value_environment_in(flow->execution, &proxy,
            storage_capture_arguments, walk, &complete, &remainder);
        if (status != XR_XIR_OK) return status;
        walk->remainder |= remainder;
        walk->complete &= complete;
    }
    if (flow->status != XR_XIR_OK) return flow->status;
    if (!walk->complete) return XR_XIR_UNRESOLVED;
    for (uint32_t n = 0; n < walk->count && storage_step(flow); ++n) {
        if (walk->items[n].kind != STORAGE_CARRIER_CELL) continue;
        XirStorageReference reference;
        XrXirStatus status = storage_current_reference(flow, point->function, walk->items[n].reference, &reference);
        if (status != XR_XIR_OK) return status;
        walk->items[n].reference = reference;
    }
    return flow->status;
}

static void storage_capture_clear(StorageCaptureWalk *walk) {
    xr_compile_resources_free(walk->items); xr_compile_resources_free(walk->index);
}

XR_FUNC XrXirStatus xir_storage_flow_visit_value_captures(XirStorageFlow *flow, const XirStorageValuePoint *point,
    uint32_t banks, XirStorageCaptureVisitor visit, void *context) {
    if (!point || !visit || !banks || (banks & ~XIR_STORAGE_QUERY_ALL) ||
        !storage_valid_point(flow, point->function, point->instruction, point->point))
        return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    flow->request_remainder = 0;
    StorageCaptureWalk walk = {.flow=flow, .complete=true};
    XrXirStatus status = storage_capture_collect(&walk, point);
    flow->request_remainder |= walk.remainder;
    for (uint32_t n = 0; status == XR_XIR_OK && n < walk.count; ++n) {
        if (walk.items[n].kind == STORAGE_CARRIER_FN) continue;
        if (walk.items[n].kind == STORAGE_CARRIER_STORABLE) {
            XirStorageCapture stored = {.identity=walk.items[n].reference, .function=point->function,
                .contents=UINT32_MAX, .exposure=UINT32_MAX, .kind=XIR_STORAGE_CAPTURE_STORABLE_FN};
            status = visit(context, &stored);
            continue;
        }
        if (walk.items[n].kind == STORAGE_CARRIER_FORMAL) {
            XirStorageCapture formal = {.identity=walk.items[n].reference, .function=point->function,
                .contents=banks & XIR_STORAGE_QUERY_CONTENTS ? walk.items[n].proxy : UINT32_MAX,
                .exposure=UINT32_MAX, .kind=XIR_STORAGE_CAPTURE_FORMAL_FN};
            status = visit(context, &formal);
            continue;
        }
        XirStorageCapture capture = {.identity=walk.items[n].reference, .function=point->function,
            .contents=UINT32_MAX, .exposure=UINT32_MAX};
        XirStorageObservation observation = {point->function, point->instruction, point->point, capture.identity};
        status = xir_storage_flow_reference(flow, &observation,
            banks & XIR_STORAGE_QUERY_CONTENTS ? &capture.contents : NULL,
            banks & XIR_STORAGE_QUERY_EXPOSURE ? &capture.exposure : NULL);
        if (status == XR_XIR_OK) status = visit(context, &capture);
    }
    storage_capture_clear(&walk);
    return status;
}

static XrXirStatus storage_publish_value(XirStorageFlow *flow, const XirStorageValuePoint *point,
    const XirStorageObservation *condition) {
    if (!point || !storage_valid_point(flow, point->function, point->instruction, point->point) ||
        flow->exposure_started) return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    flow->request_remainder = 0;
    XirStorageReference destination = {0};
    if (condition) {
        if (condition->function != point->function || !storage_valid_point(flow,
            condition->function, condition->instruction, condition->point)) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status = storage_current_reference(flow, condition->function, condition->reference, &destination);
        if (status != XR_XIR_OK) return status;
    }
    StorageCaptureWalk walk = {.flow=flow, .complete=true};
    XrXirStatus status = storage_capture_collect(&walk, point);
    flow->request_remainder |= walk.remainder;
    for (uint32_t n = 0; status == XR_XIR_OK && n < walk.count; ++n) {
        if (walk.items[n].kind != STORAGE_CARRIER_CELL) continue;
        if (!storage_record_publication(flow, point->function, (StoragePublication){
            .point=point->instruction, .handle=walk.items[n].reference.value, .kind=point->point,
            .condition_point=condition ? condition->instruction : 0, .condition_handle=destination.value,
            .condition_kind=condition ? condition->point : XIR_STORAGE_BEFORE, .conditional=condition != NULL}))
            status = flow->status;
    }
    storage_capture_clear(&walk);
    return status;
}

XR_FUNC XrXirStatus xir_storage_flow_expose_value(XirStorageFlow *flow, const XirStorageValuePoint *point) {
    return storage_publish_value(flow, point, NULL);
}

XR_FUNC XrXirStatus xir_storage_flow_expose_value_when(XirStorageFlow *flow, const XirStorageValuePoint *point,
    const XirStorageObservation *condition) {
    if (!condition) return XR_XIR_BAD_STRUCTURE;
    return storage_publish_value(flow, point, condition);
}

XR_FUNC uint32_t xir_storage_flow_remainder(const XirStorageFlow *flow) {
    return flow ? flow->request_remainder : XIR_EXECUTION_REMAINDER_OPAQUE;
}
