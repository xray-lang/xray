/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_point.inc.c - Owned numeric callable observation points
 *
 * KEY CONCEPT:
 *   Equal restriction values retain separate observation points. Context and
 *   storage identities never borrow a caller instruction or an array member.
 */

static XrXirStatus storage_reference_pending(XirStorageFlow *flow, uint32_t remainder) {
    flow->request_remainder |= remainder;
    return XR_XIR_UNRESOLVED;
}

static XrXirStatus storage_local_reference(XirStorageFlow *flow, uint32_t function,
    XirStorageReference reference, bool cell, XirStorageReference *output) {
    for (uint32_t step = 0; step <= flow->view.application_count && storage_step(flow); ++step) {
        if (reference.function >= flow->view.function_count ||
            reference.value >= flow->view.functions[reference.function].value_count) return XR_XIR_BAD_STRUCTURE;
        if (reference.environment == XIR_EXECUTION_CURRENT_ENVIRONMENT) {
            if (reference.function != function || (cell && !storage_is_handle(flow, function, reference.value)))
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

static XrXirStatus storage_current_reference(XirStorageFlow *flow, uint32_t function,
    XirStorageReference reference, XirStorageReference *output) {
    return storage_local_reference(flow, function, reference, true, output);
}

static XrXirStatus storage_history_identity(XirStorageFlow *flow, XirStorageReference *reference) {
    StorageAddress address;
    if (!storage_address(flow,reference->function,reference->value,&address)) return flow->status;
    if (address.count != 1) return storage_reference_pending(flow,XIR_EXECUTION_REMAINDER_OPAQUE);
    StorageFunction *row = &flow->rows[reference->function];
    uint32_t root = row->address_values[address.first];
    if (root < row->body->parameter_count) return storage_reference_pending(flow,XIR_EXECUTION_REMAINDER_OPAQUE);
    XrXirOp operation = row->operations[root-row->body->parameter_count].op;
    if (operation != XR_XIR_CELL_NEW && operation != XR_XIR_CELL_NEW_TAKE)
        return storage_reference_pending(flow,XIR_EXECUTION_REMAINDER_OPAQUE);
    reference->value = root;
    return XR_XIR_OK;
}

static uint32_t storage_point_value_hash(XirStorageValuePoint point) {
    uint32_t fields[] = {point.function, point.instruction, (uint32_t)point.point,
        point.reference.function, point.reference.value, point.reference.environment};
    return xr_hash_bytes(fields, sizeof(fields));
}

static uint32_t storage_point_proxy_hash(uint32_t function, uint32_t value) {
    uint32_t fields[] = {function, value};
    return xr_hash_bytes(fields, sizeof(fields));
}

static XirStorageValuePoint storage_point_key(XirStoragePointValue value) {
    return (XirStorageValuePoint){value.function, value.instruction, value.reference, value.point};
}

static bool storage_same_point(XirStorageValuePoint a, XirStorageValuePoint b) {
    return a.function == b.function && a.instruction == b.instruction && a.point == b.point &&
        a.reference.function == b.reference.function && a.reference.value == b.reference.value &&
        a.reference.environment == b.reference.environment;
}

static bool storage_point_index_grow(XirStorageFlow *flow) {
    uint64_t next = flow->point_index_capacity ? (uint64_t)flow->point_index_capacity * 2 : 16;
    if (next > UINT32_MAX) { flow->status = XR_XIR_OVERFLOW; return false; }
    uint32_t capacity = (uint32_t)next;
    uint32_t *index = xir_compile_calloc(&flow->memory, capacity, sizeof(*index), &flow->status);
    uint32_t *values = xir_compile_calloc(&flow->memory, capacity, sizeof(*values), &flow->status);
    if (flow->status == XR_XIR_OK) {
        for (uint32_t p = 0; p < flow->view.point_value_count && storage_step(flow); ++p) {
            XirStoragePointValue point = flow->view.point_values[p];
            uint32_t slot = storage_point_value_hash(storage_point_key(point)) & (capacity - 1);
            while (index[slot]) slot = (slot + 1) & (capacity - 1);
            index[slot] = p + 1;
            slot = storage_point_proxy_hash(point.function, point.value) & (capacity - 1);
            while (values[slot]) slot = (slot + 1) & (capacity - 1);
            values[slot] = p + 1;
        }
    }
    if (flow->status != XR_XIR_OK) {
        xr_compile_resources_free(index); xr_compile_resources_free(values); return false;
    }
    xr_compile_resources_free(flow->point_index); xr_compile_resources_free(flow->point_value_index);
    flow->point_index = index; flow->point_value_index = values; flow->point_index_capacity = capacity;
    return true;
}

static uint32_t storage_existing_point(const XirStorageFlow *flow, XirStorageValuePoint point) {
    if (!flow->point_index_capacity) return UINT32_MAX;
    uint32_t slot = storage_point_value_hash(point) & (flow->point_index_capacity - 1);
    while (flow->point_index[slot]) {
        uint32_t index = flow->point_index[slot] - 1;
        if (storage_same_point(point, storage_point_key(flow->view.point_values[index]))) return index;
        slot = (slot + 1) & (flow->point_index_capacity - 1);
    }
    if (point.reference.function != point.function ||
        point.reference.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT) return UINT32_MAX;
    slot = storage_point_proxy_hash(point.function, point.reference.value) & (flow->point_index_capacity - 1);
    while (flow->point_value_index[slot]) {
        uint32_t index = flow->point_value_index[slot] - 1;
        XirStoragePointValue existing = flow->view.point_values[index];
        if (existing.function == point.function && existing.value == point.reference.value) {
            if (existing.instruction == point.instruction && existing.point == point.point) return index;
            return UINT32_MAX;
        }
        slot = (slot + 1) & (flow->point_index_capacity - 1);
    }
    return UINT32_MAX;
}

XR_FUNC XrXirStatus xir_storage_flow_point_value(XirStorageFlow *flow, const XirStorageValuePoint *point,
    XirStorageReference *output) {
    if (!point || !output || !storage_valid_point(flow, point->function, point->instruction, point->point))
        return XR_XIR_BAD_STRUCTURE;
    if (flow->status != XR_XIR_OK) return flow->status;
    flow->request_remainder = 0;
    uint32_t index = storage_existing_point(flow, *point);
    if (index != UINT32_MAX) {
        *output = (XirStorageReference){point->function, flow->view.point_values[index].value,
            XIR_EXECUTION_CURRENT_ENVIRONMENT};
        return XR_XIR_OK;
    }
    if (flow->exposure_started) return XR_XIR_BAD_STRUCTURE;
    XirStorageReference source;
    XrXirStatus status = storage_local_reference(flow, point->function, point->reference, false, &source);
    if (status != XR_XIR_OK) return status;
    if (!flow->point_index_capacity || flow->view.point_value_count >= flow->point_index_capacity / 2)
        if (!storage_point_index_grow(flow)) return flow->status;
    void *points = (void *)flow->view.point_values;
    if (!storage_reserve(flow, &points, flow->view.point_value_count, &flow->point_value_capacity,
        sizeof(*flow->view.point_values))) return flow->status;
    flow->view.point_values = points;
    uint32_t value = storage_aux(flow, point->function);
    if (value == UINT32_MAX || !storage_relation(flow, point->function, source.value, value)) return flow->status;
    index = flow->view.point_value_count++;
    ((XirStoragePointValue *)points)[index] = (XirStoragePointValue){
        point->function, value, point->instruction, point->point, point->reference, UINT32_MAX};
    uint32_t slot = storage_point_value_hash(*point) & (flow->point_index_capacity - 1);
    while (flow->point_index[slot]) slot = (slot + 1) & (flow->point_index_capacity - 1);
    flow->point_index[slot] = index + 1;
    slot = storage_point_proxy_hash(point->function, value) & (flow->point_index_capacity - 1);
    while (flow->point_value_index[slot]) slot = (slot + 1) & (flow->point_index_capacity - 1);
    flow->point_value_index[slot] = index + 1;
    *output = (XirStorageReference){point->function, value, XIR_EXECUTION_CURRENT_ENVIRONMENT};
    return XR_XIR_OK;
}

static XrXirStatus storage_formal_footprint(XirStorageFlow *flow, const XirStorageValuePoint *point,
    uint32_t parameter, uint32_t source, bool entry, XirStorageReference *output) {
    if (parameter >= flow->rows[point->function].body->parameter_count) return XR_XIR_BAD_STRUCTURE;
    XirStorageValuePoint formal = *point;
    formal.reference = (XirStorageReference){point->function, source, XIR_EXECUTION_CURRENT_ENVIRONMENT};
    XrXirStatus status = xir_storage_flow_point_value(flow, &formal, output);
    if (status != XR_XIR_OK) return status;
    uint32_t index = storage_existing_point(flow, formal);
    if (index == UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    if (flow->view.point_values[index].footprint_input != UINT32_MAX) return XR_XIR_OK;
    if (flow->exposure_started) return XR_XIR_BAD_STRUCTURE;
    StorageFunction *row = &flow->rows[point->function];
    if (!storage_reserve(flow, (void **)&row->footprint_inputs, row->footprint_input_count,
        &row->footprint_input_capacity, sizeof(*row->footprint_inputs))) return flow->status;
    uint32_t value = storage_aux(flow, point->function), input = flow->view.input_count;
    if (value == UINT32_MAX || !storage_input(flow, point->function, value, parameter, XIR_STORAGE_FOOTPRINT) ||
        !storage_relation_bank(flow, point->function, value, output->value, XIR_STORAGE_FOOTPRINT)) return flow->status;
    ((XirStorageInput *)flow->view.inputs)[input].entry_environment = entry;
    if (source != parameter && !storage_relation_bank(flow, point->function,
        parameter, output->value, XIR_STORAGE_FOOTPRINT)) return flow->status;
    uint32_t link = row->footprint_input_count++;
    row->footprint_inputs[link] = (StorageFootprintLink){input, row->footprint_heads[parameter]};
    row->footprint_heads[parameter] = link;
    ((XirStoragePointValue *)flow->view.point_values)[index].footprint_input = value;
    ++flow->revision;
    return XR_XIR_OK;
}

static bool storage_entry_environment(const XirStorageFlow *flow, const XirStorageValuePoint *point) {
    const StorageFunction *row = &flow->rows[point->function];
    if (row->blocks[point->instruction] || row->body->blocks[0].first ||
        row->predecessors[0] != UINT32_MAX) return false;
    if (point->point == XIR_STORAGE_BEFORE) return point->instruction <= row->first_environment_effect;
    return point->point == XIR_STORAGE_NORMAL && point->instruction < row->first_environment_effect;
}

/* A stored value keeps an external environment loan independently of its
 * restriction bound. Callable ROOT and suspension uppers stay on the value. */
static XrXirStatus storage_storable_footprint(XirStorageFlow *flow,
    const XirStorageValuePoint *point, XirStorageReference proxy) {
    XirStorageValuePoint key = *point;
    key.reference = proxy;
    uint32_t index = storage_existing_point(flow,key);
    if (index == UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    if (flow->view.point_values[index].footprint_input != UINT32_MAX) return XR_XIR_OK;
    if (flow->exposure_started) return XR_XIR_BAD_STRUCTURE;
    uint32_t value = storage_aux(flow,point->function);
    if (value == UINT32_MAX || !storage_unknown(flow,point->function,value,XR_XIR_UNIT) ||
        !storage_relation_bank(flow,point->function,value,proxy.value,XIR_STORAGE_FOOTPRINT)) return flow->status;
    ((XirStoragePointValue *)flow->view.point_values)[index].footprint_input = value;
    return XR_XIR_OK;
}
