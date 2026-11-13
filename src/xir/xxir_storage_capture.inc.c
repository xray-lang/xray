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
    if ((uint32_t)observation->age > XIR_STORAGE_HISTORICAL) return XR_XIR_BAD_STRUCTURE;
    if (observation->age == XIR_STORAGE_HISTORICAL) {
        if (observation->point != XIR_STORAGE_BEFORE && observation->point != XIR_STORAGE_NORMAL)
            return storage_reference_pending(flow,XIR_EXECUTION_REMAINDER_OPAQUE);
        status = storage_history_identity(flow,&reference);
        if (status != XR_XIR_OK) return status;
        if (exposure && !storage_start_exposure(flow)) return flow->status;
        uint32_t read = contents ? storage_history_point(flow,observation->function,observation->instruction,
            reference.value,observation->point,XIR_STORAGE_CONTENTS) : UINT32_MAX;
        uint32_t published = exposure ? storage_history_point(flow,observation->function,observation->instruction,
            reference.value,observation->point,XIR_STORAGE_EXPOSURE) : UINT32_MAX;
        if (flow->status != XR_XIR_OK || !storage_close_applications(flow)) return flow->status;
        if (contents) *contents = read;
        if (exposure) *exposure = published;
        return XR_XIR_OK;
    }
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
    if ((uint32_t)observation->age > XIR_STORAGE_HISTORICAL) return XR_XIR_BAD_STRUCTURE;
    XirStorageReference reference;
    XrXirStatus status = storage_current_reference(flow, observation->function, observation->reference, &reference);
    if (status == XR_XIR_OK && observation->age == XIR_STORAGE_HISTORICAL) {
        if (flow->exposure_started || (observation->point != XIR_STORAGE_BEFORE && observation->point != XIR_STORAGE_NORMAL))
            return XR_XIR_BAD_STRUCTURE;
        status = storage_history_identity(flow,&reference);
        if (status != XR_XIR_OK) return status;
        storage_record_publication(flow,observation->function,(StoragePublication){.point=observation->instruction,
            .handle=reference.value,.kind=observation->point,.historical=true});
        return flow->status;
    }
    return status == XR_XIR_OK ? xir_storage_flow_expose(flow, observation->function,
        observation->instruction, reference.value, observation->point) : status;
}

typedef enum StorageCarrierKind {
    STORAGE_CARRIER_FN, STORAGE_CARRIER_CELL, STORAGE_CARRIER_FORMAL, STORAGE_CARRIER_STORABLE,
    STORAGE_CARRIER_HIDDEN, STORAGE_CARRIER_OPAQUE, STORAGE_CARRIER_CONTENTS
} StorageCarrierKind;
typedef struct StorageCaptureItem {
    XirStorageReference reference;
    StorageCarrierKind kind;
    uint32_t proxy;
    XirStorageIdentityAge age;
    bool storable, retained;
} StorageCaptureItem;
typedef struct StorageCaptureWalk {
    XirStorageFlow *flow;
    StorageCaptureItem *items;
    uint32_t *index;
    uint32_t count, capacity, index_capacity, remainder;
    XirStorageReference current_proxy, current_reference;
    XirStorageValuePoint point;
    XirStorageFunctionVisitor function_visit;
    void *function_context;
    bool complete, current_storable;
} StorageCaptureWalk;

static uint32_t storage_capture_hash(StorageCaptureItem item) {
    uint32_t fields[] = {item.reference.function, item.reference.value, item.reference.environment, (uint32_t)item.kind, item.age,
        item.storable,
        item.kind == STORAGE_CARRIER_FORMAL || item.kind == STORAGE_CARRIER_HIDDEN ||
            item.kind == STORAGE_CARRIER_CONTENTS || item.storable ? item.proxy : 0};
    return xr_hash_bytes(fields, sizeof(fields));
}

/* Capture sites provide identities for the symbolic definition. Execution
 * sites supply the current contents and environment through their bindings. */
static bool storage_environment_entry(XirStorageFlow *flow, const XirExecutionSite *input,
    const XirStorageValuePoint *point) {
    return storage_entry_environment(flow,point) && storage_identity_input(flow,input);
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
        if (found.kind == item.kind && found.age == item.age && found.storable == item.storable &&
            ((item.kind != STORAGE_CARRIER_FORMAL && item.kind != STORAGE_CARRIER_HIDDEN &&
                item.kind != STORAGE_CARRIER_CONTENTS && !item.storable) || found.proxy == item.proxy) &&
            found.reference.function == item.reference.function &&
            found.reference.value == item.reference.value && found.reference.environment == item.reference.environment) return true;
        slot = (slot + 1) & (walk->index_capacity - 1);
    }
    if (!storage_reserve(flow, (void **)&walk->items, walk->count, &walk->capacity, sizeof(*walk->items))) return false;
    walk->items[walk->count] = item; walk->index[slot] = ++walk->count;
    return true;
}

static XrXirStatus storage_capture_function(StorageCaptureWalk *walk, const XirExecutionSite *site) {
    if (!walk->function_visit || site->protocol != 1) return XR_XIR_OK;
    XirStorageFlow *flow = walk->flow;
    if (site->function >= flow->view.function_count) return XR_XIR_BAD_STRUCTURE;
    const StorageFunction *row = &flow->rows[site->function];
    if (site->instruction >= row->body->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (row->operations[site->instruction].op != XR_XIR_FUNCTION_REF) return XR_XIR_OK;
    uint32_t value = row->body->parameter_count + site->instruction;
    XirStorageFunctionOccurrence occurrence = {.constructor={site->function, value, site->environment}, .ages=1u,
        .constructor_call=site->index, .caller_instance=site->caller_instance, .entry_context=site->entry_context};
    if (site->environment == XIR_EXECUTION_UNKNOWN_ENVIRONMENT) {
        occurrence.ages = 0;
        occurrence.remainder = XIR_EXECUTION_REMAINDER_STORAGE;
    }
    occurrence.repeated = storage_allocation_repeats(flow, site->function, value);
    if (flow->status != XR_XIR_OK) return flow->status;
    if (occurrence.repeated) {
        occurrence.ages = 0;
        if (site->function == walk->point.function && site->environment == XIR_EXECUTION_CURRENT_ENVIRONMENT) {
            uint32_t saved_remainder = flow->request_remainder;
            XrXirStatus status = storage_occurrence_ages(flow, &walk->point, value, value, &occurrence.ages);
            if (status != XR_XIR_OK && status != XR_XIR_UNRESOLVED) return status;
            if (status == XR_XIR_UNRESOLVED) occurrence.remainder = flow->request_remainder;
            flow->request_remainder = saved_remainder;
        } else occurrence.remainder = XIR_EXECUTION_REMAINDER_STORAGE;
    }
    return walk->function_visit(walk->function_context, &occurrence);
}

static XrXirStatus storage_capture_arguments(void *opaque, const XirExecutionSite *capture) {
    StorageCaptureWalk *walk = opaque;
    XirStorageFlow *flow = walk->flow;
    XrXirStatus observed = storage_capture_function(walk, capture);
    if (observed != XR_XIR_OK) return observed;
    if (capture->kind == XIR_EXECUTION_STORABLE) {
        if (capture->argument_count || capture->receiver_function >= flow->view.function_count ||
            capture->receiver_value >= flow->view.functions[capture->receiver_function].value_count ||
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
        XirExecutionParameter parameter = {0};
        status = xir_execution_parameter(flow->execution, capture, 0, &parameter);
        if (status != XR_XIR_OK) return status;
        XirStorageReference proxy = walk->current_proxy;
        if (!parameter.generic_storage &&
            !xr_xir_type_is_c_callable(xir_execution_types(flow->execution), parameter.type)) {
            XirStorageReference local;
            status = storage_local_reference(flow, walk->point.function,
                (XirStorageReference){formal.function, formal.value, formal.environment}, false, &local);
            if (status != XR_XIR_OK) return status;
            status = storage_formal_footprint(flow, &walk->point, local.value, local.value, false, &proxy);
            if (status != XR_XIR_OK) return status;
        }
        if (!storage_capture_push(walk, (StorageCaptureItem){
            .reference={formal.function, formal.value, formal.environment},
            .kind=STORAGE_CARRIER_FORMAL, .proxy=proxy.value})) return flow->status;
        return XR_XIR_OK;
    }
    if (capture->kind == XIR_EXECUTION_CONTENTS_INPUT) {
        XirExecutionArgument formal = {0};
        XrXirStatus status = xir_execution_argument(flow->execution, capture, 0, &formal);
        if (status != XR_XIR_OK) return status;
        if (!formal.cell || formal.function != walk->point.function ||
            formal.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT)
            return storage_reference_pending(flow, XIR_EXECUTION_REMAINDER_OPAQUE);
        bool entry = storage_environment_entry(flow, capture, &walk->point);
        if (flow->status != XR_XIR_OK) return flow->status;
        XirStorageReference proxy;
        if (entry) status = storage_formal_footprint(flow, &walk->point, formal.value,
            capture->receiver_value, true, &proxy);
        else {
            if (!storage_cell_closed_environments(flow, capture))
                return flow->status == XR_XIR_OK ?
                    storage_reference_pending(flow, XIR_EXECUTION_REMAINDER_OPAQUE) : flow->status;
            XirStorageValuePoint point = walk->point;
            point.reference = (XirStorageReference){formal.function, capture->receiver_value, formal.environment};
            status = xir_storage_flow_point_value(flow, &point, &proxy);
            if (status == XR_XIR_OK && !storage_relation_bank(flow, point.function,
                formal.value, proxy.value, XIR_STORAGE_FOOTPRINT)) status = flow->status;
        }
        if (status != XR_XIR_OK) return status;
        return storage_capture_push(walk, (StorageCaptureItem){
            .reference={formal.function, formal.value, formal.environment}, .kind=STORAGE_CARRIER_CONTENTS,
            .proxy=proxy.value}) ? XR_XIR_OK : flow->status;
    }
    if (capture->kind == XIR_EXECUTION_CAPTURE_INPUT) {
        XrXirStatus status = storage_hidden_restriction(flow, capture);
        if (status != XR_XIR_OK) return status;
        XirExecutionArgument formal = {0};
        status = xir_execution_argument(flow->execution, capture, 0, &formal);
        if (status != XR_XIR_OK) return status;
        XirStorageReference local, proxy = {0};
        status = storage_local_reference(flow, walk->point.function,
            (XirStorageReference){formal.function, formal.value, formal.environment}, false, &local);
        if (status == XR_XIR_OK) status = storage_formal_footprint(flow, &walk->point, local.value, local.value, false, &proxy);
        if (status != XR_XIR_OK) return status;
        return storage_capture_push(walk, (StorageCaptureItem){
            .reference={formal.function, formal.value, formal.environment}, .kind=STORAGE_CARRIER_HIDDEN,
            .proxy=proxy.value}) ? XR_XIR_OK : flow->status;
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
        uint32_t ages = 1;
        if (!actual.cell) {
            const XrXirTypes *types = xir_execution_types(flow->execution);
            if (!xr_xir_type_is_callable(types, actual.type) || xr_xir_type_is_c_callable(types, actual.type)) continue;
        } else if (actual.function == walk->point.function &&
            actual.environment == XIR_EXECUTION_CURRENT_ENVIRONMENT && flow->rows[actual.function].cyclic) {
            uint32_t allocation = 0;
            uint32_t parameters = flow->rows[actual.function].body->parameter_count;
            if (storage_repeated_cell(flow,actual.function,actual.value,&allocation)) {
                if (capture->function != actual.function || capture->instruction == UINT32_MAX)
                    return storage_reference_pending(flow,XIR_EXECUTION_REMAINDER_OPAQUE);
                if (!storage_temporal_handle(flow,actual.function,capture->instruction,XIR_STORAGE_BEFORE,actual.value))
                    return flow->status;
                XirStorageValuePoint use = walk->point;
                use.reference = walk->current_reference;
                status = storage_occurrence_ages(flow,&use,parameters+capture->instruction,allocation,&ages);
                if (status != XR_XIR_OK) return status;
            } else if (flow->status != XR_XIR_OK) return flow->status;
        }
        for (uint32_t age = 0; age < 2; ++age) if (ages & (1u << age)) {
            if (!storage_capture_push(walk, (StorageCaptureItem){.reference={actual.function, actual.value, actual.environment},
                .kind=actual.cell ? STORAGE_CARRIER_CELL : STORAGE_CARRIER_FN,.age=(XirStorageIdentityAge)age,
                .proxy=walk->current_proxy.value,.storable=walk->current_storable || capture->storable_environment}))
                return flow->status;
        }
    }
    return flow->status;
}

static XrXirStatus storage_capture_external(StorageCaptureWalk *walk, XirStorageReference proxy) {
    XrXirStatus status = storage_storable_footprint(walk->flow, &walk->point, proxy);
    if (status != XR_XIR_OK) return status;
    return storage_capture_push(walk, (StorageCaptureItem){.reference=proxy,.kind=STORAGE_CARRIER_OPAQUE}) ?
        XR_XIR_OK : walk->flow->status;
}

static XrXirStatus storage_capture_collect(StorageCaptureWalk *walk, const XirStorageValuePoint *point) {
    XirStorageFlow *flow = walk->flow;
    walk->point = *point;
    if (!storage_capture_push(walk, (StorageCaptureItem){.reference=point->reference, .kind=STORAGE_CARRIER_FN})) return flow->status;
    for (uint32_t n = 0; n < walk->count && storage_step(flow); ++n) {
        StorageCaptureItem item = walk->items[n];
        if (item.kind == STORAGE_CARRIER_FORMAL || item.kind == STORAGE_CARRIER_HIDDEN ||
            item.kind == STORAGE_CARRIER_STORABLE || item.kind == STORAGE_CARRIER_OPAQUE ||
            item.kind == STORAGE_CARRIER_CONTENTS) continue;
        if (item.kind == STORAGE_CARRIER_CELL) {
            XirStorageReference local;
            uint32_t saved_remainder = flow->request_remainder;
            XrXirStatus status = storage_current_reference(flow, point->function, item.reference, &local);
            if (status == XR_XIR_UNRESOLVED && item.storable) {
                /* RETURN and stored sinks already publish this environment.
                 * Keep its foreign identity and opaque loan at the caller. */
                flow->request_remainder = saved_remainder;
                walk->items[n].retained = true;
                status = storage_capture_external(walk, (XirStorageReference){
                    point->function,item.proxy,XIR_EXECUTION_CURRENT_ENVIRONMENT});
                if (status != XR_XIR_OK) return status;
                continue;
            }
            if (status != XR_XIR_OK) return status;
            XrXirType payload = storage_payload(flow, local.function, local.value);
            if (!xr_xir_type_is_callable(flow->module->types, payload) ||
                xr_xir_type_is_c_callable(flow->module->types, payload)) continue;
            uint32_t contents;
            XirStorageObservation observation = {point->function,point->instruction,point->point,local,item.age};
            status = xir_storage_flow_reference(flow,&observation,&contents,NULL);
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
        walk->current_reference = item.reference;
        walk->current_storable = item.storable;
        status = xir_execution_visit_value_footprint_in(flow->execution, &proxy,
            storage_capture_arguments, walk, &complete, &remainder);
        if (status != XR_XIR_OK) return status;
        if (item.storable && (remainder & (XIR_EXECUTION_REMAINDER_PARAMETER | XIR_EXECUTION_REMAINDER_OPAQUE))) {
            remainder &= ~(XIR_EXECUTION_REMAINDER_PARAMETER | XIR_EXECUTION_REMAINDER_OPAQUE);
            remainder |= XIR_EXECUTION_REMAINDER_STORABLE_ENVIRONMENT;
            complete = false;
        }
        if (remainder & XIR_EXECUTION_REMAINDER_STORABLE_ENVIRONMENT) {
            /* The stored-value contract covers unknown environment branches.
             * Known captures continue through the ordinary identity queries. */
            status = storage_capture_external(walk,proxy);
            if (status != XR_XIR_OK) return status;
            remainder &= ~XIR_EXECUTION_REMAINDER_STORABLE_ENVIRONMENT;
            complete = !remainder;
        }
        walk->remainder |= remainder;
        walk->complete &= complete;
    }
    if (flow->status != XR_XIR_OK) return flow->status;
    if (!walk->complete) return XR_XIR_UNRESOLVED;
    for (uint32_t n = 0; n < walk->count && storage_step(flow); ++n) {
        if (walk->items[n].kind != STORAGE_CARRIER_CELL || walk->items[n].retained) continue;
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
    uint32_t banks, const XirStorageCaptureObservers *observers) {
    if (!flow || !point || !observers || (!observers->capture && !observers->function) || !banks ||
        (observers->function && flow->sealed) || (banks & ~XIR_STORAGE_QUERY_ALL) ||
        !storage_valid_point(flow, point->function, point->instruction, point->point))
        return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    flow->request_remainder = 0;
    StorageCaptureWalk walk = {.flow=flow, .complete=true,
        .function_visit=observers->function, .function_context=observers->context};
    XrXirStatus status = storage_capture_collect(&walk, point);
    flow->request_remainder |= walk.remainder;
    for (uint32_t n = 0; status == XR_XIR_OK && n < walk.count; ++n) {
        if (!observers->capture) continue;
        if (walk.items[n].kind == STORAGE_CARRIER_FN) continue;
        if (walk.items[n].retained) {
            XirStorageCapture stored = {.identity=walk.items[n].reference,.function=point->function,
                .contents=UINT32_MAX,.exposure=UINT32_MAX,.kind=XIR_STORAGE_CAPTURE_STORED_CELL,.age=walk.items[n].age};
            status = observers->capture(observers->context,&stored);
            continue;
        }
        if (walk.items[n].kind == STORAGE_CARRIER_OPAQUE) {
            XirStorageCapture opaque = {.identity=walk.items[n].reference,.function=point->function,
                .contents=walk.items[n].reference.value,.exposure=UINT32_MAX,.kind=XIR_STORAGE_CAPTURE_OPAQUE_FN};
            status = observers->capture(observers->context,&opaque);
            continue;
        }
        if (walk.items[n].kind == STORAGE_CARRIER_STORABLE) {
            XirStorageCapture stored = {.identity=walk.items[n].reference, .function=point->function,
                .contents=UINT32_MAX, .exposure=UINT32_MAX, .kind=XIR_STORAGE_CAPTURE_STORABLE_FN};
            status = observers->capture(observers->context, &stored);
            continue;
        }
        if (walk.items[n].kind == STORAGE_CARRIER_FORMAL || walk.items[n].kind == STORAGE_CARRIER_HIDDEN ||
            walk.items[n].kind == STORAGE_CARRIER_CONTENTS) {
            XirStorageCapture formal = {.identity=walk.items[n].reference, .function=point->function,
                .contents=banks & XIR_STORAGE_QUERY_CONTENTS ? walk.items[n].proxy : UINT32_MAX,
                .exposure=UINT32_MAX, .kind=walk.items[n].kind == STORAGE_CARRIER_CONTENTS ?
                    XIR_STORAGE_CAPTURE_CONTENTS_FN : walk.items[n].kind == STORAGE_CARRIER_HIDDEN ?
                        XIR_STORAGE_CAPTURE_HIDDEN_FN : XIR_STORAGE_CAPTURE_FORMAL_FN};
            status = observers->capture(observers->context, &formal);
            continue;
        }
        XirStorageCapture capture = {.identity=walk.items[n].reference, .function=point->function,
            .contents=UINT32_MAX, .exposure=UINT32_MAX,.age=walk.items[n].age};
        XirStorageObservation observation = {point->function,point->instruction,point->point,capture.identity,capture.age};
        status = xir_storage_flow_reference(flow, &observation,
            banks & XIR_STORAGE_QUERY_CONTENTS ? &capture.contents : NULL,
            banks & XIR_STORAGE_QUERY_EXPOSURE ? &capture.exposure : NULL);
        if (status == XR_XIR_OK) status = observers->capture(observers->context, &capture);
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
        if (walk.items[n].kind != STORAGE_CARRIER_CELL || walk.items[n].retained) continue;
        if (!storage_record_publication(flow, point->function, (StoragePublication){
            .point=point->instruction, .handle=walk.items[n].reference.value, .kind=point->point,
            .condition_point=condition ? condition->instruction : 0, .condition_handle=destination.value,
            .condition_kind=condition ? condition->point : XIR_STORAGE_BEFORE, .conditional=condition != NULL,
            .historical=walk.items[n].age == XIR_STORAGE_HISTORICAL}))
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

typedef struct StorageCallFootprint {
    XirStorageFlow *flow;
    uint32_t function, value;
} StorageCallFootprint;

static XrXirStatus storage_call_footprint_capture(void *opaque, const XirStorageCapture *capture) {
    StorageCallFootprint *point = opaque;
    if (capture->kind == XIR_STORAGE_CAPTURE_STORABLE_FN || capture->kind == XIR_STORAGE_CAPTURE_STORED_CELL)
        return XR_XIR_OK;
    if (capture->function != point->function || capture->contents == UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    if (capture->kind == XIR_STORAGE_CAPTURE_CELL) {
        if (capture->identity.function != point->function ||
            capture->identity.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT ||
            !storage_is_handle(point->flow, point->function, capture->identity.value)) return XR_XIR_BAD_STRUCTURE;
        if (!storage_relation_bank(point->flow, point->function, capture->identity.value,
            point->value, XIR_STORAGE_FOOTPRINT)) return point->flow->status;
    }
    return storage_relation_bank(point->flow, point->function, capture->contents,
        point->value, XIR_STORAGE_FOOTPRINT) ? XR_XIR_OK : point->flow->status;
}

static XrXirStatus storage_call_footprint(XirStorageFlow *flow, const XirExecutionSite *site,
    XirExecutionArgument actual, uint32_t *output) {
    XrXirType type = actual.cell ? xr_xir_cell_element(xir_execution_types(flow->execution), actual.type) : actual.type;
    if (!xr_xir_type_is_callable(xir_execution_types(flow->execution), type) ||
        xr_xir_type_is_c_callable(xir_execution_types(flow->execution), type)) return XR_XIR_BAD_STRUCTURE;
    if (site->kind == XIR_EXECUTION_CLEANUP_USE) {
        XirStorageReference reference = {actual.function, actual.value, actual.environment};
        if (actual.cell) {
            if (actual.caller_handle == UINT32_MAX)
                return storage_reference_pending(flow, XIR_EXECUTION_REMAINDER_OPAQUE);
            if (actual.caller_handle >= flow->view.functions[site->function].physical_values ||
                !storage_is_handle(flow, site->function, actual.caller_handle)) return XR_XIR_BAD_STRUCTURE;
            uint32_t contents = storage_call_input(flow, site, actual.caller_handle);
            if (contents == UINT32_MAX) return flow->status;
            reference = (XirStorageReference){site->function, contents, XIR_EXECUTION_CURRENT_ENVIRONMENT};
        } else if (actual.function != site->function ||
            actual.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT) {
            return storage_reference_pending(flow, XIR_EXECUTION_REMAINDER_OPAQUE);
        }
        /* The contents version already carries the actual exit port and LIFO
         * order, including panic exits without a physical instruction. Only
         * a closed value can use this bounded environment transport. */
        XrXirStatus status = storage_closed_value_environment(flow, reference);
        if (status == XR_XIR_OK && actual.cell && !storage_relation_bank(flow, site->function,
            actual.caller_handle, reference.value, XIR_STORAGE_FOOTPRINT)) status = flow->status;
        if (status == XR_XIR_OK) *output = reference.value;
        return status;
    }
    XirStorageValuePoint use = {site->function, site->instruction,
        {actual.function, actual.value, actual.environment}, XIR_STORAGE_BEFORE};
    if (actual.cell) {
        if (actual.caller_handle == UINT32_MAX) return storage_reference_pending(flow, XIR_EXECUTION_REMAINDER_OPAQUE);
        if (actual.caller_handle >= flow->view.functions[site->function].physical_values ||
            !storage_is_handle(flow,site->function,actual.caller_handle)) return XR_XIR_BAD_STRUCTURE;
        uint32_t contents = storage_call_input(flow, site, actual.caller_handle);
        if (contents == UINT32_MAX) return flow->status;
        use.reference = (XirStorageReference){site->function, contents, XIR_EXECUTION_CURRENT_ENVIRONMENT};
    }
    XirStorageReference proxy;
    XrXirStatus status = xir_storage_flow_point_value(flow, &use, &proxy);
    if (status != XR_XIR_OK) return status;
    if (actual.cell && !storage_relation_bank(flow, site->function, actual.caller_handle,
        proxy.value, XIR_STORAGE_FOOTPRINT)) return flow->status;
    StorageCallFootprint point = {flow, site->function, proxy.value};
    status = xir_storage_flow_visit_value_captures(flow, &use, XIR_STORAGE_QUERY_CONTENTS,
        &(XirStorageCaptureObservers){.context=&point, .capture=storage_call_footprint_capture});
    if (status == XR_XIR_OK) *output = proxy.value;
    return status;
}

XR_FUNC uint32_t xir_storage_flow_remainder(const XirStorageFlow *flow) {
    return flow ? flow->request_remainder : XIR_EXECUTION_REMAINDER_OPAQUE;
}
