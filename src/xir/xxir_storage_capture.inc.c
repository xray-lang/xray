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

static XrXirStatus storage_reference_pending(XirStorageFlow *flow, uint32_t remainder) {
    flow->request_remainder |= remainder;
    return XR_XIR_UNRESOLVED;
}

static XrXirStatus storage_current_reference(XirStorageFlow *flow, uint32_t function,
    XirStorageReference reference, XirStorageReference *output) {
    for (uint32_t step = 0; step <= flow->view.application_count && storage_step(flow); ++step) {
        if (reference.function >= flow->view.function_count ||
            reference.value >= flow->view.functions[reference.function].value_count) return XR_XIR_BAD_STRUCTURE;
        if (reference.environment == XIR_EXECUTION_CURRENT_ENVIRONMENT) {
            if (reference.function != function || !storage_is_handle(flow, function, reference.value))
                return storage_reference_pending(flow, XIR_EXECUTION_REMAINDER_OPAQUE);
            *output = reference;
            return XR_XIR_OK;
        }
        if (reference.environment == XIR_EXECUTION_UNKNOWN_ENVIRONMENT)
            return storage_reference_pending(flow, XIR_EXECUTION_REMAINDER_OPAQUE);
        if (reference.environment >= flow->view.application_count) return XR_XIR_BAD_STRUCTURE;
        XirStorageApplication app = flow->view.applications[reference.environment];
        if (app.declaration != reference.function) return storage_reference_pending(flow, XIR_EXECUTION_REMAINDER_OPAQUE);
        if (app.first > flow->view.binding_count || app.count > flow->view.binding_count - app.first)
            return XR_XIR_BAD_STRUCTURE;
        bool found = false;
        for (uint32_t b = 0; b < app.count && storage_step(flow); ++b) {
            XirStorageBinding binding = flow->view.bindings[app.first + b];
            if (binding.formal_function != reference.function || binding.formal_value != reference.value) continue;
            reference = (XirStorageReference){binding.actual_function, binding.actual_value,
                binding.actual_environment == XIR_EXECUTION_CURRENT_ENVIRONMENT ? app.environment : binding.actual_environment};
            found = true;
            break;
        }
        if (flow->status != XR_XIR_OK) return flow->status;
        if (!found) return storage_reference_pending(flow, XIR_EXECUTION_REMAINDER_OPAQUE);
    }
    return flow->status == XR_XIR_OK ?
        storage_reference_pending(flow, XIR_EXECUTION_REMAINDER_OPAQUE) : flow->status;
}

static bool storage_valid_point(const XirStorageFlow *flow, uint32_t function,
    uint32_t instruction, XirStoragePointKind point) {
    return flow && !flow->sealed && flow->local_finished && function < flow->view.function_count &&
        instruction < flow->rows[function].body->instruction_count && (uint32_t)point <= XIR_STORAGE_PANIC;
}

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

typedef struct StorageCaptureItem { XirStorageReference reference; bool cell; } StorageCaptureItem;
typedef struct StorageCaptureWalk {
    XirStorageFlow *flow;
    StorageCaptureItem *items;
    uint32_t *index;
    uint32_t count, capacity, index_capacity;
    bool complete;
} StorageCaptureWalk;

static uint32_t storage_capture_hash(StorageCaptureItem item) {
    uint32_t fields[] = {item.reference.function, item.reference.value, item.reference.environment, item.cell};
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
        if (found.cell == item.cell && found.reference.function == item.reference.function &&
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
    for (uint32_t p = 0; p < capture->argument_count && storage_step(flow); ++p) {
        XirExecutionArgument actual = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, capture, p, &actual);
        if (status == XR_XIR_UNRESOLVED) {
            flow->request_remainder |= XIR_EXECUTION_REMAINDER_STORAGE;
            walk->complete = false;
            continue;
        }
        if (status != XR_XIR_OK) return status;
        if (!actual.cell && !xr_xir_callable_signature(xir_execution_types(flow->execution), actual.type)) continue;
        if (!storage_capture_push(walk, (StorageCaptureItem){{actual.function, actual.value, actual.environment}, actual.cell}))
            return flow->status;
    }
    return flow->status;
}

static XrXirStatus storage_capture_collect(StorageCaptureWalk *walk, const XirStorageValuePoint *point) {
    XirStorageFlow *flow = walk->flow;
    if (!storage_capture_push(walk, (StorageCaptureItem){point->reference, false})) return flow->status;
    for (uint32_t n = 0; n < walk->count && storage_step(flow); ++n) {
        StorageCaptureItem item = walk->items[n];
        if (item.cell) {
            XirStorageReference local;
            XrXirStatus status = storage_current_reference(flow, point->function, item.reference, &local);
            if (status != XR_XIR_OK) return status;
            if (!xr_xir_callable_signature(flow->module->types, storage_payload(flow, local.function, local.value))) continue;
            uint32_t contents;
            status = xir_storage_flow_contents_at(flow, point->function, point->instruction,
                local.value, point->point, &contents);
            if (status != XR_XIR_OK) return status;
            if (!storage_capture_push(walk, (StorageCaptureItem){
                {point->function, contents, XIR_EXECUTION_CURRENT_ENVIRONMENT}, false})) return flow->status;
            continue;
        }
        bool complete = false;
        uint32_t remainder = 0;
        XrXirStatus status = xir_execution_visit_value_captures_in(flow->execution, &item.reference,
            storage_capture_arguments, walk, &complete, &remainder);
        if (status != XR_XIR_OK) return status;
        flow->request_remainder |= remainder;
        walk->complete &= complete;
    }
    if (flow->status != XR_XIR_OK) return flow->status;
    if (!walk->complete) return XR_XIR_UNRESOLVED;
    for (uint32_t n = 0; n < walk->count && storage_step(flow); ++n) {
        if (!walk->items[n].cell) continue;
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
    for (uint32_t n = 0; status == XR_XIR_OK && n < walk.count; ++n) {
        if (!walk.items[n].cell) continue;
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
    for (uint32_t n = 0; status == XR_XIR_OK && n < walk.count; ++n) {
        if (!walk.items[n].cell) continue;
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
