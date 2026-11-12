/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage_occurrence.inc.c - Allocation ages of reaching value occurrences
 *
 * KEY CONCEPT:
 *   The execution owner selects a callable occurrence. This query follows its
 *   existing value reads and storage transfers, projecting age for one captured
 *   allocation without selecting callees or enumerating dynamic activations.
 */
typedef struct StorageOccurrence {
    uint32_t value, point;
    bool contents, after, historical;
} StorageOccurrence;

typedef struct StorageOccurrenceQuery {
    XirStorageFlow *flow;
    uint32_t function, allocation, occurrence, ages, remainder;
    StorageOccurrence *values;
    uint32_t *index, count, capacity, index_capacity;
} StorageOccurrenceQuery;

static bool storage_allocation_repeats(XirStorageFlow *flow, uint32_t function,
                                       uint32_t allocation) {
    StorageFunction *row = &flow->rows[function];
    if (!row->cyclic)
        return false;
    uint32_t physical = flow->view.functions[function].physical_values;
    XR_DCHECK(allocation >= row->body->parameter_count && allocation < physical,
              "allocation age belongs to this template frame");
    if (!row->allocation_cycles) {
        row->allocation_cycles = xir_compile_calloc(&flow->memory, physical,
                                                    sizeof(*row->allocation_cycles), &flow->status);
        if (!row->allocation_cycles)
            return false;
    }
    if (row->allocation_cycles[allocation])
        return row->allocation_cycles[allocation] == 2;
    if (++row->generation == 0) {
        memset(row->visited, 0, (size_t) physical * sizeof(*row->visited));
        row->generation = 1;
    }
    uint32_t point = allocation - row->body->parameter_count;
    uint32_t first = 0, count = 1;
    row->queue[0] = point;
    row->visited[point] = row->generation;
    bool repeated = false;
    while (first < count && !repeated && storage_step(flow)) {
        uint32_t current = row->queue[first++];
        for (uint32_t e = row->predecessors[current]; e != UINT32_MAX && storage_step(flow);
             e = row->edges[e].next) {
            StoragePredecessor edge = row->edges[e];
            if (edge.source == point) {
                repeated = true;
                break;
            }
            if (row->visited[edge.source] == row->generation)
                continue;
            row->visited[edge.source] = row->generation;
            row->queue[count++] = edge.source;
        }
    }
    if (flow->status == XR_XIR_OK)
        row->allocation_cycles[allocation] = repeated ? 2 : 1;
    return repeated;
}

static bool storage_repeated_cell(XirStorageFlow *flow, uint32_t function, uint32_t handle,
                                  uint32_t *allocation) {
    StorageAddress address;
    if (!storage_address(flow, function, handle, &address))
        return false;
    if (address.count != 1) {
        flow->status = XR_XIR_UNRESOLVED;
        return false;
    }
    uint32_t root = flow->rows[function].address_values[address.first];
    uint32_t parameters = flow->rows[function].body->parameter_count;
    if (root < parameters)
        return false;
    XrXirOp operation = flow->rows[function].operations[root - parameters].op;
    if (operation != XR_XIR_CELL_NEW && operation != XR_XIR_CELL_NEW_TAKE)
        return false;
    if (!storage_allocation_repeats(flow, function, root))
        return false;
    *allocation = root;
    return true;
}

static uint32_t storage_occurrence_hash(StorageOccurrence value) {
    uint32_t fields[] = {value.value, value.point, value.contents, value.after, value.historical};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool storage_occurrence_index(StorageOccurrenceQuery *query) {
    XirStorageFlow *flow = query->flow;
    uint64_t next = query->index_capacity ? (uint64_t) query->index_capacity * 2 : 16;
    if (next > UINT32_MAX) {
        flow->status = XR_XIR_OVERFLOW;
        return false;
    }
    uint32_t *index =
        xir_compile_calloc(&flow->memory, (size_t) next, sizeof(*index), &flow->status);
    if (!index)
        return false;
    for (uint32_t v = 0; v < query->count && storage_step(flow); ++v) {
        uint32_t slot = storage_occurrence_hash(query->values[v]) & ((uint32_t) next - 1);
        while (index[slot] && storage_step(flow))
            slot = (slot + 1) & ((uint32_t) next - 1);
        index[slot] = v + 1;
    }
    if (flow->status != XR_XIR_OK) {
        xr_compile_resources_free(index);
        return false;
    }
    xr_compile_resources_free(query->index);
    query->index = index;
    query->index_capacity = (uint32_t) next;
    return true;
}

static bool storage_occurrence_push(StorageOccurrenceQuery *query, StorageOccurrence value) {
    XirStorageFlow *flow = query->flow;
    XR_DCHECK(value.value < flow->view.functions[query->function].physical_values &&
                  value.point < flow->rows[query->function].point_count,
              "occurrence uses owned numeric coordinates");
    if (!query->index_capacity || query->count >= query->index_capacity / 2)
        if (!storage_occurrence_index(query))
            return false;
    uint32_t slot = storage_occurrence_hash(value) & (query->index_capacity - 1);
    while (query->index[slot] && storage_step(flow)) {
        StorageOccurrence found = query->values[query->index[slot] - 1];
        if (found.value == value.value && found.point == value.point &&
            found.contents == value.contents && found.after == value.after &&
            found.historical == value.historical)
            return true;
        slot = (slot + 1) & (query->index_capacity - 1);
    }
    if (flow->status != XR_XIR_OK || !storage_reserve(flow, (void **) &query->values, query->count,
                                                      &query->capacity, sizeof(*query->values)))
        return false;
    query->values[query->count] = value;
    query->index[slot] = ++query->count;
    return true;
}

static bool storage_occurrence_source(StorageOccurrenceQuery *query, StorageRead read,
                                      bool historical) {
    if (read.kind != XIR_STORAGE_BEFORE && read.kind != XIR_STORAGE_NORMAL) {
        query->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
        return true;
    }
    if (read.force) {
        StorageAddress address;
        if (!storage_address(query->flow, query->function, read.source, &address))
            return false;
        if (address.count != 1) {
            query->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
            return true;
        }
        read.source = query->flow->rows[query->function].address_values[address.first];
    }
    return storage_occurrence_push(query, (StorageOccurrence) {read.source, read.point, read.force,
                                                               read.kind == XIR_STORAGE_NORMAL,
                                                               historical});
}

static bool storage_occurrence_definition(StorageOccurrenceQuery *query, StorageOccurrence value) {
    XirStorageFlow *flow = query->flow;
    StorageFunction *row = &flow->rows[query->function];
    XR_DCHECK(value.value >= row->body->parameter_count,
              "definition query names an instruction result");
    if (value.value == query->occurrence) {
        query->ages |= 1u << (value.historical ? XIR_STORAGE_HISTORICAL : XIR_STORAGE_RECENT);
        return true;
    }
    XrXirOp op = row->operations[value.value - row->body->parameter_count].op;
    if (storage_copy(op) || op == XR_XIR_CELL_PLACE || op == XR_XIR_CELL_PROJECT) {
        uint32_t instruction = value.value - row->body->parameter_count;
        return storage_occurrence_source(query,
                                         (StorageRead) {row->operations[instruction].source, 0,
                                                        instruction, XIR_STORAGE_BEFORE, false},
                                         value.historical);
    }
    uint32_t first = 0, end = row->read_count;
    while (first < end) {
        uint32_t middle = first + (end - first) / 2;
        if (row->reads[middle].target < value.value)
            first = middle + 1;
        else
            end = middle;
    }
    bool transported = first < row->read_count && row->reads[first].target == value.value;
    for (uint32_t r = first;
         r < row->read_count && row->reads[r].target == value.value && storage_step(flow); ++r)
        if (!storage_occurrence_source(query, row->reads[r], value.historical))
            return false;
    if (!transported && op != XR_XIR_FUNCTION_REF)
        query->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
    return flow->status == XR_XIR_OK;
}

static bool storage_occurrence_edge(StorageOccurrenceQuery *query, StorageOccurrence value,
                                    StoragePredecessor edge) {
    XirStorageFlow *flow = query->flow;
    StorageFunction *row = &flow->rows[query->function];
    if (edge.kind == XIR_INSTRUCTION_FLOW_PANIC) {
        query->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
        return true;
    }
    uint32_t output = row->body->parameter_count + edge.source;
    StorageOperation op = row->operations[edge.source];
    if (op.op == XR_XIR_BRANCH && edge.kind == XIR_INSTRUCTION_FLOW_TARGET &&
        op.source >= row->body->parameter_count) {
        uint8_t truth = row->operations[op.source - row->body->parameter_count].truth;
        /* Literal feasibility refines this property over the shared CFG. */
        if (truth && edge.ordinal != (truth == STORAGE_TRUTH_TRUE ? 0u : 1u))
            return true;
    }
    value.after = false;
    value.point = edge.source;
    if (!value.contents && value.value == output)
        return storage_occurrence_definition(query, value);
    value.historical |= output == query->allocation;
    if (!value.contents) {
        return storage_occurrence_push(query, value);
    }
    if (storage_new_place(op.op) && value.value == output)
        return storage_occurrence_source(
            query, (StorageRead) {op.source, 0, edge.source, XIR_STORAGE_BEFORE, false},
            value.historical);
    if (storage_write(op.op)) {
        StorageAddress address;
        if (!storage_address(flow, query->function, op.destination, &address))
            return false;
        for (uint32_t a = 0; a < address.count && storage_step(flow); ++a) {
            if (row->address_values[address.first + a] != value.value)
                continue;
            if (!storage_occurrence_source(
                    query, (StorageRead) {op.source, 0, edge.source, XIR_STORAGE_BEFORE, false},
                    value.historical))
                return false;
            if (address.count == 1)
                return true;
        }
    }
    if (storage_unmodeled_resume(op.op) ||
        (storage_execution(op.op) && row->site_heads[edge.source] != UINT32_MAX)) {
        query->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
        return true;
    }
    return storage_occurrence_push(query, value);
}

static bool storage_occurrence_expand(StorageOccurrenceQuery *query, StorageOccurrence value) {
    XirStorageFlow *flow = query->flow;
    StorageFunction *row = &flow->rows[query->function];
    if (!value.contents && value.value >= row->body->parameter_count &&
        value.point == value.value - row->body->parameter_count)
        return storage_occurrence_definition(query, value);
    if (value.after)
        return storage_occurrence_edge(
            query, value,
            (StoragePredecessor) {value.point, UINT32_MAX, 0, XIR_INSTRUCTION_FLOW_NEXT});
    if (!value.point && value.value < row->body->parameter_count)
        query->remainder |= XIR_EXECUTION_REMAINDER_PARAMETER;
    for (uint32_t e = row->predecessors[value.point]; e != UINT32_MAX && storage_step(flow);
         e = row->edges[e].next)
        if (!storage_occurrence_edge(query, value, row->edges[e]))
            return false;
    return flow->status == XR_XIR_OK;
}

static bool storage_occurrence_start(StorageOccurrenceQuery *query,
                                     const XirStorageValuePoint *point) {
    XirStorageFlow *flow = query->flow;
    XirStorageValuePoint source = *point;
    StorageFunction *row = &flow->rows[query->function];
    uint32_t physical = flow->view.functions[query->function].physical_values;
    for (uint32_t step = 0; step <= flow->view.point_value_count && storage_step(flow); ++step) {
        XirStorageReference reference = source.reference;
        if (reference.function != query->function ||
            reference.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT)
            break;
        if (reference.value < physical)
            return storage_occurrence_push(
                query, (StorageOccurrence) {reference.value, source.instruction, false,
                                            source.point == XIR_STORAGE_NORMAL, false});
        uint32_t first = 0, end = row->state_count;
        while (first < end) {
            uint32_t middle = first + (end - first) / 2;
            if (row->states[middle].value < reference.value)
                first = middle + 1;
            else
                end = middle;
        }
        if (first < row->state_count && row->states[first].value == reference.value) {
            StorageState state = row->states[first];
            if (state.kind & STORAGE_HISTORY_KIND)
                break;
            uint32_t kind = storage_base_kind(state);
            if (storage_bank(state) == XIR_STORAGE_CONTENTS && (kind == 0 || kind == 5))
                return storage_occurrence_push(
                    query, (StorageOccurrence) {state.handle, state.point, true, kind == 5, false});
        }
        uint32_t index = storage_existing_point(flow, source);
        if (index == UINT32_MAX)
            break;
        source = storage_point_key(flow->view.point_values[index]);
        if (source.reference.value >= reference.value)
            break;
    }
    query->remainder |= XIR_EXECUTION_REMAINDER_OPAQUE;
    return true;
}

static XrXirStatus storage_occurrence_ages(XirStorageFlow *flow, const XirStorageValuePoint *point,
                                           uint32_t occurrence, uint32_t allocation,
                                           uint32_t *ages) {
    if (point->point != XIR_STORAGE_BEFORE && point->point != XIR_STORAGE_NORMAL)
        return storage_reference_pending(flow, XIR_EXECUTION_REMAINDER_OPAQUE);
    StorageOccurrenceQuery query = {.flow = flow,
                                    .function = point->function,
                                    .allocation = allocation,
                                    .occurrence = occurrence};
    storage_occurrence_start(&query, point);
    for (uint32_t n = 0; n < query.count && storage_step(flow); ++n)
        if (!storage_occurrence_expand(&query, query.values[n]))
            break;
    XrXirStatus status = flow->status;
    if (status == XR_XIR_OK && (query.remainder || !query.ages))
        status = storage_reference_pending(flow, query.remainder ? query.remainder
                                                                 : XIR_EXECUTION_REMAINDER_OPAQUE);
    if (status == XR_XIR_OK)
        *ages = query.ages;
    xr_compile_resources_free(query.values);
    xr_compile_resources_free(query.index);
    return status;
}

static bool storage_temporal_handle(XirStorageFlow *flow, uint32_t function, uint32_t instruction,
                                    XirStoragePointKind kind, uint32_t handle) {
    if (!flow->rows[function].cyclic)
        return true;
    StorageAddress address;
    if (!storage_address(flow, function, handle, &address))
        return false;
    for (uint32_t a = 0; a < address.count && storage_step(flow); ++a) {
        uint32_t root = flow->rows[function].address_values[address.first + a];
        if (root == handle || root < flow->rows[function].body->parameter_count)
            continue;
        XrXirOp operation =
            flow->rows[function].operations[root - flow->rows[function].body->parameter_count].op;
        if ((operation != XR_XIR_CELL_NEW && operation != XR_XIR_CELL_NEW_TAKE) ||
            !storage_allocation_repeats(flow, function, root))
            continue;
        XirStorageValuePoint point = {
            function, instruction, {function, handle, XIR_EXECUTION_CURRENT_ENVIRONMENT}, kind};
        uint32_t ages = 0;
        XrXirStatus status = storage_occurrence_ages(flow, &point, root, root, &ages);
        if (status != XR_XIR_OK) {
            flow->status = status;
            return false;
        }
        if (ages != 1) {
            flow->status = XR_XIR_UNRESOLVED;
            return false;
        }
    }
    return flow->status == XR_XIR_OK;
}

static XrXirStatus storage_call_capture_age(XirStorageFlow *flow, const XirExecutionSite *site,
                                            XirExecutionArgument actual, uint32_t *ages) {
    uint32_t allocation = 0;
    uint32_t parameters = flow->rows[actual.function].body->parameter_count;
    if (!storage_repeated_cell(flow, actual.function, actual.value, &allocation)) {
        *ages = 1;
        return flow->status;
    }
    if (site->reference == UINT32_MAX || site->receiver_function != site->function ||
        actual.function != site->function ||
        actual.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT ||
        site->capture_environment != XIR_EXECUTION_CURRENT_ENVIRONMENT)
        return storage_reference_pending(flow, XIR_EXECUTION_REMAINDER_OPAQUE);
    XirExecutionSite capture = {0};
    XrXirStatus status =
        xir_execution_site(flow->execution, XIR_EXECUTION_CALL, site->reference, &capture);
    if (status != XR_XIR_OK)
        return status;
    if (capture.function != site->function || capture.instruction == UINT32_MAX)
        return storage_reference_pending(flow, XIR_EXECUTION_REMAINDER_OPAQUE);
    XirStorageValuePoint point = {
        site->function,
        site->instruction,
        {site->receiver_function, site->receiver_value, site->environment},
        XIR_STORAGE_BEFORE};
    return storage_occurrence_ages(flow, &point, parameters + capture.instruction, allocation,
                                   ages);
}
