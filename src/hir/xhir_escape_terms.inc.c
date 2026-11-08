/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_escape_terms.inc.c - Positive contextual callable lifetime equations
 *
 * KEY CONCEPT:
 *   Symbolic environment inputs are replaced at each application. Storage
 *   supplies reaching versions and identity relations, never permissions.
 */
typedef struct HirRestrictionNode {
    uint32_t *inputs, count, capacity;
    uint32_t head, parameter, borrowed, version;
    bool constant, queued, allocated;
} HirRestrictionNode;
typedef struct HirRestrictionEdge {
    uint32_t source, target, next, application, seen;
    bool dependency;
} HirRestrictionEdge;
typedef struct HirRestrictionCarrier { uint32_t function, source, observation; } HirRestrictionCarrier;
typedef struct HirRestrictionCheck { uint32_t use, carrier, exposure; } HirRestrictionCheck;
typedef struct HirRestrictionSolver {
    XrHirEscapeBuilder *builder;
    XirStorageFlow *storage;
    const XirExecutionOwner *execution;
    XirStorageFlowView view;
    HirRestrictionNode *nodes;
    HirRestrictionEdge *edges;
    HirRestrictionCarrier *carriers;
    HirRestrictionCheck *checks;
    uint32_t *offsets, *counts, *point_values, *queue;
    uint32_t count, edge_count, edge_capacity, carrier_count, carrier_capacity;
    uint32_t check_count, check_capacity, queue_first, queue_count;
    uint32_t observation;
    HirEscapeObservation *base_observations;
    uint32_t base_observation_count, base_use_count;
    uint32_t remainder;
    bool ready, final;
} HirRestrictionSolver;

static bool restriction_empty(const HirRestrictionNode *node) {
    return !node->constant && !node->count;
}

static XrXirStatus restriction_storage_status(HirRestrictionSolver *solver, XrXirStatus status) {
    if (status == XR_XIR_UNRESOLVED) solver->remainder = xir_storage_flow_remainder(solver->storage);
    return status;
}

static void restriction_free(HirRestrictionSolver *solver) {
    for (uint32_t n = 0; n < solver->count; ++n)
        xr_compile_resources_free(solver->nodes ? solver->nodes[n].inputs : NULL);
    xr_compile_resources_free(solver->nodes); xr_compile_resources_free(solver->edges);
    xr_compile_resources_free(solver->carriers); xr_compile_resources_free(solver->checks);
    xr_compile_resources_free(solver->offsets); xr_compile_resources_free(solver->counts);
    xr_compile_resources_free(solver->point_values);
    xr_compile_resources_free(solver->queue);
    xr_compile_resources_free(solver->base_observations);
}

static void escape_points_free(XrHirEscapeBuilder *builder) {
    if (!builder->point_preparation) return;
    restriction_free(builder->point_preparation);
    xr_compile_resources_free(builder->point_preparation);
    builder->point_preparation = NULL;
}

static uint32_t restriction_node(HirRestrictionSolver *solver, uint32_t f, uint32_t value, bool exposure) {
    HirEscape *owner = &solver->builder->graph;
    if (f >= solver->view.function_count || value >= solver->counts[f]) {
        owner->status = XR_XIR_BAD_STRUCTURE;
        return UINT32_MAX;
    }
    uint32_t index = solver->offsets[f] + value;
    return exposure ? index + solver->offsets[solver->view.function_count] : index;
}

static void restriction_enqueue(HirRestrictionSolver *solver, uint32_t value) {
    if (solver->nodes[value].queued) return;
    solver->queue[(solver->queue_first + solver->queue_count) % solver->count] = value;
    ++solver->queue_count;
    solver->nodes[value].queued = true;
}

static bool restriction_input(HirRestrictionSolver *solver, uint32_t index, uint32_t input) {
    HirEscape *owner = &solver->builder->graph;
    HirRestrictionNode *node = &solver->nodes[index];
    uint32_t first = 0, end = node->count;
    while (first < end) {
        uint32_t middle = first + (end - first) / 2;
        if (node->inputs[middle] < input) first = middle + 1;
        else end = middle;
    }
    if (first < node->count && node->inputs[first] == input) return false;
    if (node->count == node->capacity) {
        uint32_t *grown = xir_compile_array_grow(owner->memory, node->inputs,
            node->count, sizeof(*grown), &node->capacity, &owner->status);
        if (!grown) return false;
        node->inputs = grown;
    }
    memmove(node->inputs + first + 1, node->inputs + first, (size_t)(node->count - first) * sizeof(*node->inputs));
    node->inputs[first] = input;
    ++node->count;
    return true;
}

static bool restriction_merge(HirRestrictionSolver *solver, uint32_t target,
    const HirRestrictionNode *source) {
    HirRestrictionNode *node = &solver->nodes[target];
    bool changed = false;
    if (source->constant && !node->constant) {
        node->constant = true; node->parameter = source->parameter; node->borrowed = source->borrowed;
        changed = true;
    }
    if (source->parameter && node->constant && !node->parameter) {
        node->parameter = source->parameter;
        changed = true;
    }
    if (!node->constant) for (uint32_t p = 0; p < source->count; ++p)
        changed |= restriction_input(solver, target, source->inputs[p]);
    if (changed) {++node->version; restriction_enqueue(solver, target);}
    return changed;
}

static bool restriction_edge(HirRestrictionSolver *solver, uint32_t source, uint32_t target,
    uint32_t application, bool dependency) {
    HirEscape *owner = &solver->builder->graph;
    if (source >= solver->count || target >= solver->count) {owner->status = XR_XIR_BAD_STRUCTURE; return false;}
    if (solver->edge_count == solver->edge_capacity) {
        HirRestrictionEdge *grown = xir_compile_array_grow(owner->memory, solver->edges,
            solver->edge_count, sizeof(*grown), &solver->edge_capacity, &owner->status);
        if (!grown) return false;
        solver->edges = grown;
    }
    uint32_t index = solver->edge_count++;
    solver->edges[index] = (HirRestrictionEdge){source, target, solver->nodes[source].head,
        application, 0, dependency};
    solver->nodes[source].head = index;
    return true;
}

static bool restriction_local_edge(HirRestrictionSolver *solver, uint32_t f,
    uint32_t source, uint32_t target, bool exposure) {
    uint32_t from = restriction_node(solver, f, source, exposure), to = restriction_node(solver, f, target, exposure);
    return solver->builder->graph.status == XR_XIR_OK && restriction_edge(solver, from, to, UINT32_MAX, false);
}

static XrXirStatus restriction_capture(void *context, const XirStorageCapture *capture) {
    HirRestrictionSolver *solver = context;
    HirEscape *owner = &solver->builder->graph;
    if (capture->function != solver->builder->observations[solver->observation].function) return XR_XIR_BAD_STRUCTURE;
    if (capture->kind == XIR_STORAGE_CAPTURE_STORABLE_FN)
        return capture->contents == UINT32_MAX && capture->exposure == UINT32_MAX ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
    if (capture->contents == UINT32_MAX) return XR_XIR_UNRESOLVED;
    if (capture->kind == XIR_STORAGE_CAPTURE_FORMAL_FN || capture->kind == XIR_STORAGE_CAPTURE_HIDDEN_FN) {
        uint32_t f = capture->identity.function, p = capture->identity.value;
        if (capture->exposure != UINT32_MAX || f >= owner->module->function_count ||
            p >= owner->functions[f].parameters ||
            (p < owner->functions[f].captures) != (capture->kind == XIR_STORAGE_CAPTURE_HIDDEN_FN))
            return XR_XIR_BAD_STRUCTURE;
        XrXirType type = owner->nodes[owner->offsets[f] + p].type;
        if (!xr_xir_callable_signature(owner->module->types, type) || xr_xir_type_is_c_callable(owner->module->types, type))
            return XR_XIR_BAD_STRUCTURE;
    } else if (capture->kind != XIR_STORAGE_CAPTURE_CELL) return XR_XIR_BAD_STRUCTURE;
    if (solver->carrier_count == solver->carrier_capacity) {
        HirRestrictionCarrier *grown = xir_compile_array_grow(owner->memory, solver->carriers,
            solver->carrier_count, sizeof(*grown), &solver->carrier_capacity, &owner->status);
        if (!grown) return owner->status;
        solver->carriers = grown;
    }
    solver->carriers[solver->carrier_count++] = (HirRestrictionCarrier){capture->function,
        capture->contents, solver->observation};
    return XR_XIR_OK;
}

static bool restriction_append_observation(XrHirEscapeBuilder *builder, HirEscapeObservation row) {
    HirEscape *owner = &builder->graph;
    if (builder->observation_count == builder->observation_capacity) {
        HirEscapeObservation *grown = xir_compile_array_grow(owner->memory, builder->observations,
            builder->observation_count, sizeof(*grown), &builder->observation_capacity, &owner->status);
        if (!grown) return false;
        builder->observations = grown;
    }
    builder->observations[builder->observation_count++] = row;
    return true;
}

static int restriction_observation_compare(const void *left, const void *right) {
    const HirEscapeObservation *a = left, *b = right;
    if (a->function != b->function) return a->function < b->function ? -1 : 1;
    if (a->instruction != b->instruction) return a->instruction < b->instruction ? -1 : 1;
    if (a->value != b->value) return a->value < b->value ? -1 : 1;
    return 0;
}

static XrXirStatus restriction_call_actuals(void *context, const XirExecutionSite *site) {
    HirRestrictionSolver *solver = context;
    HirEscape *owner = &solver->builder->graph;
    for (uint32_t p = site->capture_count; p < site->argument_count && escape_poll(owner); ++p) {
        XirExecutionParameter formal;
        XrXirStatus status = xir_execution_parameter(solver->execution, site, p, &formal);
        if (status != XR_XIR_OK) return status;
        if (formal.hidden || (!formal.cell && !formal.generic_storage)) continue;
        XirExecutionArgument actual;
        status = xir_execution_argument(solver->execution, site, p, &actual);
        if (status != XR_XIR_OK) return status;
        const XrXirTypes *types = xir_execution_types(solver->execution);
        bool cell = formal.cell || actual.cell;
        XrXirType type = formal.cell ? formal.type : actual.type;
        if (xr_xir_type_is_cell(types, type)) type = xr_xir_cell_element(types, type);
        if (!xr_xir_callable_signature(types, type) || xr_xir_type_is_c_callable(types, type)) continue;
        XirStorageReference reference = {actual.function, actual.value, actual.environment};
        uint32_t value = UINT32_MAX;
        const char *reason = "a generic or stored value parameter";
        if (cell) {
            /* A bound ordinary ref still names a storable variable when its
             * contents change between environment construction and invocation. */
            XirStorageObservation observation = {site->function, site->instruction, XIR_STORAGE_BEFORE, reference};
            status = restriction_storage_status(solver,
                xir_storage_flow_reference(solver->storage, &observation, &value, NULL));
            reason = "a ref or stored place";
        } else {
            XirStorageValuePoint point = {site->function, site->instruction, reference, XIR_STORAGE_BEFORE};
            XirStorageReference proxy;
            status = restriction_storage_status(solver, xir_storage_flow_point_value(solver->storage, &point, &proxy));
            if (status == XR_XIR_OK) {
                if (proxy.function != site->function || proxy.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT)
                    return XR_XIR_UNRESOLVED;
                value = proxy.value;
            }
        }
        if (status != XR_XIR_OK) return status;
        status = escape_use(owner, site->function, site->instruction, value, UINT32_MAX, reason);
        if (status != XR_XIR_OK || !restriction_append_observation(solver->builder,
            (HirEscapeObservation){site->function, site->instruction, value, XIR_STORAGE_BEFORE})) return owner->status;
    }
    return owner->status;
}

static bool restriction_function_use(const HirEscape *owner, HirEscapeUse use) {
    if (use.kind == HIR_ESCAPE_CELL_PUBLICATION) return true;
    if (use.value >= owner->functions[use.function].physical_values) return true;
    XrXirType type = owner->nodes[owner->offsets[use.function] + use.value].type;
    return xr_xir_callable_signature(owner->module->types, type) &&
        !xr_xir_type_is_c_callable(owner->module->types, type);
}

static bool restriction_cell_publications(HirRestrictionSolver *solver) {
    HirEscape *owner = &solver->builder->graph;
    uint32_t count = owner->use_count;
    for (uint32_t u = 0; u < count && escape_poll(owner); ++u) {
        HirEscapeUse use = owner->uses[u];
        if (use.kind != HIR_ESCAPE_CELL_PUBLICATION) continue;
        XrXirType type = owner->nodes[owner->offsets[use.function] + use.value].type;
        if (xr_xir_type_is_cell(owner->module->types, type)) type = xr_xir_cell_element(owner->module->types, type);
        if (!xr_xir_callable_signature(owner->module->types, type) || xr_xir_type_is_c_callable(owner->module->types, type)) continue;
        XirStorageObservation observation = {use.function, use.instruction, use.point,
            {use.function, use.value, XIR_EXECUTION_CURRENT_ENVIRONMENT}};
        uint32_t contents = UINT32_MAX;
        owner->status = restriction_storage_status(solver,
            xir_storage_flow_reference(solver->storage, &observation, &contents, NULL));
        if (owner->status != XR_XIR_OK) return false;
        owner->status = escape_use(owner, use.function, use.instruction, contents, UINT32_MAX, use.reason);
        if (owner->status != XR_XIR_OK || !restriction_append_observation(solver->builder,
            (HirEscapeObservation){use.function, use.instruction, contents, use.point})) return false;
    }
    return owner->status == XR_XIR_OK;
}

static bool restriction_observe_storage(HirRestrictionSolver *solver, bool final) {
    XrHirEscapeBuilder *builder = solver->builder;
    HirEscape *owner = &builder->graph;
    owner->status = xir_execution_visit_calls(solver->execution, restriction_call_actuals, solver);
    if (owner->status == XR_XIR_OK)
        owner->status = xir_execution_visit_captures(solver->execution, restriction_call_actuals, solver);
    if (owner->status == XR_XIR_OK) restriction_cell_publications(solver);
    if (owner->status == XR_XIR_OK && builder->observation_count) {
        qsort(builder->observations, builder->observation_count, sizeof(*builder->observations), restriction_observation_compare);
        uint32_t count = 0;
        for (uint32_t p = 0; p < builder->observation_count; ++p) {
            HirEscapeObservation row = builder->observations[p];
            if (count && !restriction_observation_compare(&builder->observations[count - 1], &row)) {
                if (builder->observations[count - 1].point != row.point) {owner->status = XR_XIR_UNRESOLVED; break;}
            } else builder->observations[count++] = row;
        }
        builder->observation_count = count;
    }
    for (uint32_t u = 0; u < owner->use_count && escape_poll(owner); ++u) {
        HirEscapeUse use = owner->uses[u];
        if (!restriction_function_use(owner, use)) continue;
        if (use.kind == HIR_ESCAPE_CELL_PUBLICATION) {
            XirStorageObservation publication = {use.function, use.instruction, XIR_STORAGE_NORMAL,
                {use.function, use.value, XIR_EXECUTION_CURRENT_ENVIRONMENT}};
            owner->status = restriction_storage_status(solver,
                xir_storage_flow_expose_reference(solver->storage, &publication));
            continue;
        }
        XirStorageValuePoint point = {use.function, use.instruction,
            {use.function, use.value, XIR_EXECUTION_CURRENT_ENVIRONMENT},
            use.point};
        if (use.storage == UINT32_MAX) owner->status = restriction_storage_status(solver,
            xir_storage_flow_expose_value(solver->storage, &point));
        else {
            point.point = XIR_STORAGE_NORMAL;
            XirStorageObservation condition = {use.function, use.instruction, XIR_STORAGE_BEFORE,
                {use.function, use.storage - owner->offsets[use.function], XIR_EXECUTION_CURRENT_ENVIRONMENT}};
            owner->status = restriction_storage_status(solver,
                xir_storage_flow_expose_value_when(solver->storage, &point, &condition));
        }
    }
    for (uint32_t p = 0; p < builder->observation_count && escape_poll(owner); ++p) {
        HirEscapeObservation row = builder->observations[p];
        XirStorageValuePoint point = {row.function, row.instruction,
            {row.function, row.value, XIR_EXECUTION_CURRENT_ENVIRONMENT}, row.point};
        solver->observation = p;
        owner->status = restriction_storage_status(solver, xir_storage_flow_visit_value_captures(solver->storage, &point,
            final ? XIR_STORAGE_QUERY_ALL : XIR_STORAGE_QUERY_CONTENTS, restriction_capture, solver));
    }
    for (uint32_t u = 0; final && u < owner->use_count && escape_poll(owner); ++u) {
        HirEscapeUse use = owner->uses[u];
        if (!restriction_function_use(owner, use)) continue;
        uint32_t exposure = UINT32_MAX;
        if (use.storage != UINT32_MAX) {
            uint32_t handle = use.storage - owner->offsets[use.function];
            owner->status = restriction_storage_status(solver, xir_storage_flow_exposure_at(solver->storage, use.function,
                use.instruction, handle, XIR_STORAGE_BEFORE, &exposure));
            if (owner->status != XR_XIR_OK) break;
        }
        if (solver->check_count == solver->check_capacity) {
            HirRestrictionCheck *grown = xir_compile_array_grow(owner->memory, solver->checks,
                solver->check_count, sizeof(*grown), &solver->check_capacity, &owner->status);
            if (!grown) break;
            solver->checks = grown;
        }
        solver->checks[solver->check_count++] = (HirRestrictionCheck){u, UINT32_MAX, exposure};
    }
    if (owner->status == XR_XIR_OK) owner->status = xir_storage_flow_view(solver->storage, &solver->view);
    return owner->status == XR_XIR_OK;
}

static bool restriction_allocate(HirRestrictionSolver *solver) {
    HirEscape *owner = &solver->builder->graph;
    uint32_t functions = owner->module->function_count;
    if (solver->view.function_count != functions) {owner->status = XR_XIR_BAD_STRUCTURE; return false;}
    solver->offsets = xir_compile_calloc(owner->memory, (size_t)functions + 1, sizeof(*solver->offsets), &owner->status);
    solver->counts = xir_compile_calloc(owner->memory, functions, sizeof(*solver->counts), &owner->status);
    solver->point_values = xir_compile_calloc(owner->memory, solver->builder->observation_count,
        sizeof(*solver->point_values), &owner->status);
    if (owner->status != XR_XIR_OK) return false;
    for (uint32_t f = 0; f < functions; ++f) {
        if (solver->view.functions[f].physical_values != owner->functions[f].physical_values ||
            solver->view.functions[f].value_count < solver->view.functions[f].physical_values) {
            owner->status = XR_XIR_BAD_STRUCTURE; return false;
        }
        solver->counts[f] = solver->view.functions[f].value_count;
    }
    for (uint32_t p = 0; p < solver->builder->observation_count; ++p) {
        uint32_t f = solver->builder->observations[p].function;
        if (solver->counts[f] == UINT32_MAX) {owner->status = XR_XIR_OVERFLOW; return false;}
        solver->point_values[p] = solver->counts[f]++;
    }
    uint64_t count = 0;
    for (uint32_t f = 0; f < functions; ++f) {
        count += solver->counts[f];
        if (count > UINT32_MAX / 2) {owner->status = XR_XIR_OVERFLOW; return false;}
        solver->offsets[f + 1] = (uint32_t)count;
    }
    solver->count = (uint32_t)count * 2;
    solver->nodes = xir_compile_calloc(owner->memory, solver->count, sizeof(*solver->nodes), &owner->status);
    solver->queue = xir_compile_calloc(owner->memory, solver->count, sizeof(*solver->queue), &owner->status);
    if (owner->status != XR_XIR_OK) return false;
    for (uint32_t n = 0; n < solver->count; ++n) {
        solver->nodes[n].head = UINT32_MAX;
        solver->nodes[n].version = 1;
    }
    return true;
}

static bool restriction_seed(HirRestrictionSolver *solver) {
    HirEscape *owner = &solver->builder->graph;
    for (uint32_t f = 0; f < solver->view.function_count && escape_poll(owner); ++f) {
        HirEscapeFunction row = owner->functions[f];
        for (uint32_t p = 0; p < row.parameters; ++p) {
            HirEscapeNode old = owner->nodes[owner->offsets[f] + p];
            uint32_t index = restriction_node(solver, f, p, false);
            HirRestrictionNode *node = &solver->nodes[index];
            bool callable = xr_xir_callable_signature(owner->module->types, old.type) != NULL &&
                !xr_xir_type_is_c_callable(owner->module->types, old.type);
            bool cell = xr_xir_type_is_cell(owner->module->types, old.type);
            if (p < row.captures && (callable || cell)) restriction_input(solver, index, p);
            else if (old.parameter) {node->constant = true; node->parameter = old.parameter;}
            else if (cell) {node->constant = true; node->borrowed = owner->offsets[f] + p + 1;}
        }
        for (uint32_t e = 0; e < solver->view.functions[f].flow_count && escape_poll(owner); ++e) {
            XrXirValueFlow edge = solver->view.functions[f].flows[e];
            if (!restriction_local_edge(solver, f, edge.source, edge.target, false)) return false;
        }
        for (uint32_t e = 0; e < solver->view.functions[f].exposure_flow_count && escape_poll(owner); ++e) {
            XrXirValueFlow edge = solver->view.functions[f].exposure_flows[e];
            if (!restriction_local_edge(solver, f, edge.source, edge.target, true)) return false;
        }
        for (uint32_t e = 0; e < solver->view.functions[f].footprint_flow_count && escape_poll(owner); ++e) {
            XrXirValueFlow edge = solver->view.functions[f].footprint_flows[e];
            if (!restriction_local_edge(solver, f, edge.source, edge.target, false)) return false;
        }
        uint32_t first = owner->offsets[f], end = owner->offsets[f + 1];
        for (uint32_t v = first; v < end && escape_poll(owner); ++v)
            for (uint32_t e = owner->nodes[v].head; e != UINT32_MAX; e = owner->edges[e].next) {
                HirEscapeEdge edge = owner->edges[e];
                if (edge.target >= first && edge.target < end &&
                    !restriction_local_edge(solver, f, v - first, edge.target - first, false)) return false;
            }
    }
    for (uint32_t c = 0; c < owner->capture_count && escape_poll(owner); ++c) {
        HirEscapeCapture capture = owner->captures[c];
        uint32_t f = 0, end = solver->view.function_count;
        while (f < end) {
            uint32_t middle = f + (end - f) / 2;
            if (owner->offsets[middle + 1] <= capture.result) f = middle + 1;
            else end = middle;
        }
        HirRestrictionNode *node = &solver->nodes[restriction_node(solver, f, capture.result - owner->offsets[f], false)];
        node->constant = true; node->borrowed = capture.source + 1;
    }
    for (uint32_t n = 0; n < solver->view.input_count && escape_poll(owner); ++n) {
        XirStorageInput input = solver->view.inputs[n];
        if (input.bank > XIR_STORAGE_FOOTPRINT || input.function >= solver->view.function_count ||
            input.handle >= owner->functions[input.function].parameters) {
            owner->status = XR_XIR_BAD_STRUCTURE; return false;
        }
        uint32_t index = restriction_node(solver, input.function, input.value, input.bank == XIR_STORAGE_EXPOSURE);
        if (owner->status != XR_XIR_OK) return false;
        bool hidden = input.handle < owner->functions[input.function].captures;
        if (input.bank == XIR_STORAGE_FOOTPRINT) {
            XrXirType type = owner->nodes[owner->offsets[input.function] + input.handle].type;
            if (input.value < owner->functions[input.function].physical_values ||
                !xr_xir_callable_signature(owner->module->types, type) ||
                xr_xir_type_is_c_callable(owner->module->types, type) ||
                (!hidden && xr_xir_function_parameter_escaping(&owner->module->functions[input.function], input.handle))) {
                owner->status = XR_XIR_BAD_STRUCTURE; return false;
            }
            restriction_input(solver, index, input.value);
        } else if (input.bank == XIR_STORAGE_EXPOSURE) {
            if (hidden) restriction_input(solver, index, input.value);
            else solver->nodes[index].constant = true;
        } else if (hidden) {
            XrXirType type = owner->nodes[owner->offsets[input.function] + input.handle].type;
            if (xr_xir_type_is_cell(owner->module->types, type)) type = xr_xir_cell_element(owner->module->types, type);
            if (xr_xir_callable_signature(owner->module->types, type) &&
                !xr_xir_type_is_c_callable(owner->module->types, type)) restriction_input(solver, index, input.value);
        }
    }
    for (uint32_t a = 0; a < solver->view.allocation_count && escape_poll(owner); ++a) {
        XirStorageAllocation allocation = solver->view.allocations[a];
        uint32_t index = restriction_node(solver, allocation.function, allocation.value, false);
        if (owner->status != XR_XIR_OK) return false;
        solver->nodes[index].allocated = true;
    }
    for (uint32_t n = 0; n < solver->view.exposure_seed_count && escape_poll(owner); ++n) {
        XirStorageExposureSeed seed = solver->view.exposure_seeds[n];
        uint32_t index = restriction_node(solver, seed.function, seed.value, true);
        if (owner->status != XR_XIR_OK) return false;
        solver->nodes[index].constant = true;
    }
    for (uint32_t p = 0; p < solver->builder->observation_count && escape_poll(owner); ++p) {
        HirEscapeObservation point = solver->builder->observations[p];
        if (!restriction_local_edge(solver, point.function, point.value, solver->point_values[p], false)) return false;
    }
    for (uint32_t c = 0; c < solver->carrier_count && escape_poll(owner); ++c) {
        HirRestrictionCarrier carrier = solver->carriers[c];
        if (!restriction_local_edge(solver, carrier.function, carrier.source,
            solver->point_values[carrier.observation], false)) return false;
    }
    return owner->status == XR_XIR_OK;
}

static bool restriction_app_edge(HirRestrictionSolver *solver, uint32_t source, uint32_t target,
    uint32_t application, bool exposure) {
    HirEscape *owner = &solver->builder->graph;
    if (application >= solver->view.application_count) {owner->status = XR_XIR_BAD_STRUCTURE; return false;}
    XirStorageApplication app = solver->view.applications[application];
    if (app.first > solver->view.binding_count || app.count > solver->view.binding_count - app.first) {
        owner->status = XR_XIR_BAD_STRUCTURE; return false;
    }
    if (!restriction_edge(solver, source, target, application, false)) return false;
    for (uint32_t b = 0; b < app.count; ++b) {
        XirStorageBinding binding = solver->view.bindings[app.first + b];
        uint32_t actual = restriction_node(solver, binding.actual_function, binding.actual_value, exposure);
        if (owner->status != XR_XIR_OK || !restriction_edge(solver, actual, source, UINT32_MAX, true)) return false;
    }
    return true;
}

static bool restriction_applications(HirRestrictionSolver *solver) {
    HirEscape *owner = &solver->builder->graph;
    for (uint32_t c = 0; c < solver->view.crossflow_count && escape_poll(owner); ++c) {
        XirStorageCrossFlow flow = solver->view.crossflows[c];
        if (flow.bank > XIR_STORAGE_FOOTPRINT) {owner->status = XR_XIR_BAD_STRUCTURE; return false;}
        bool exposure = flow.bank == XIR_STORAGE_EXPOSURE;
        uint32_t source = restriction_node(solver, flow.source_function, flow.source, exposure);
        uint32_t target = restriction_node(solver, flow.target_function, flow.target, exposure);
        if (owner->status != XR_XIR_OK) return false;
        if (flow.application == UINT32_MAX) {
            if (flow.source_function != flow.target_function) {owner->status = XR_XIR_UNRESOLVED; return false;}
            if (!restriction_edge(solver, source, target, UINT32_MAX, false)) return false;
        } else if (!restriction_app_edge(solver, source, target, flow.application, exposure)) return false;
    }
    for (uint32_t a = 0; a < solver->view.application_count && escape_poll(owner); ++a) {
        XirStorageApplication app = solver->view.applications[a];
        if (app.function >= solver->view.function_count || app.declaration >= solver->view.function_count) {
            owner->status = XR_XIR_BAD_STRUCTURE; return false;
        }
        if (app.first > solver->view.binding_count || app.count > solver->view.binding_count - app.first) {
            owner->status = XR_XIR_BAD_STRUCTURE; return false;
        }
    }
    return owner->status == XR_XIR_OK;
}

static bool restriction_substitute(HirRestrictionSolver *solver, HirRestrictionEdge edge) {
    HirEscape *owner = &solver->builder->graph;
    HirRestrictionNode *source = &solver->nodes[edge.source];
    XirStorageApplication app = solver->view.applications[edge.application];
    if (source->constant) {restriction_merge(solver, edge.target, source); return true;}
    bool exposure = edge.source >= solver->offsets[solver->view.function_count];
    uint32_t local = exposure ? edge.source - solver->offsets[solver->view.function_count] : edge.source;
    if (source->count && (local < solver->offsets[app.declaration] || local >= solver->offsets[app.declaration + 1])) {
        owner->status = XR_XIR_UNRESOLVED; return false;
    }
    for (uint32_t p = 0; p < source->count && escape_poll(owner); ++p) {
        bool found = false;
        for (uint32_t b = 0; b < app.count; ++b) {
            XirStorageBinding binding = solver->view.bindings[app.first + b];
            if (binding.formal_function != app.declaration || binding.formal_value != source->inputs[p]) continue;
            found = true;
            uint32_t index = restriction_node(solver, binding.actual_function, binding.actual_value, exposure);
            if (owner->status != XR_XIR_OK) return false;
            HirRestrictionNode actual = solver->nodes[index];
            if (!restriction_empty(&actual) && (binding.actual_function != app.function ||
                binding.actual_environment != XIR_EXECUTION_CURRENT_ENVIRONMENT) && !actual.constant) {
                owner->status = XR_XIR_UNRESOLVED; return false;
            }
            restriction_merge(solver, edge.target, &actual);
        }
        if (!found) {owner->status = XR_XIR_UNRESOLVED; return false;}
    }
    return owner->status == XR_XIR_OK;
}

static bool restriction_close(HirRestrictionSolver *solver) {
    HirEscape *owner = &solver->builder->graph;
    for (uint32_t n = 0; n < solver->count; ++n)
        if (!restriction_empty(&solver->nodes[n])) restriction_enqueue(solver, n);
    while (solver->queue_count && escape_poll(owner)) {
        uint32_t index = solver->queue[solver->queue_first];
        solver->queue_first = (solver->queue_first + 1) % solver->count;
        --solver->queue_count; solver->nodes[index].queued = false;
        for (uint32_t e = solver->nodes[index].head; e != UINT32_MAX && escape_poll(owner); e = solver->edges[e].next) {
            HirRestrictionEdge edge = solver->edges[e];
            if (edge.dependency) {
                if (edge.seen != solver->nodes[index].version) {
                    solver->edges[e].seen = solver->nodes[index].version;
                    restriction_enqueue(solver, edge.target);
                }
            } else if (edge.application != UINT32_MAX) {
                if (!restriction_substitute(solver, edge)) return false;
            } else restriction_merge(solver, edge.target, &solver->nodes[index]);
        }
    }
    return owner->status == XR_XIR_OK;
}

static uint32_t restriction_use_carrier(HirRestrictionSolver *solver, HirEscapeUse use) {
    uint32_t first = 0, end = solver->builder->observation_count;
    HirEscapeObservation key = {use.function, use.instruction, use.value, XIR_STORAGE_BEFORE};
    while (first < end) {
        uint32_t middle = first + (end - first) / 2;
        if (restriction_observation_compare(&solver->builder->observations[middle], &key) < 0) first = middle + 1;
        else end = middle;
    }
    if (first < solver->builder->observation_count &&
        !restriction_observation_compare(&solver->builder->observations[first], &key))
        return restriction_node(solver, use.function, solver->point_values[first], false);
    return restriction_node(solver, use.function, use.value, false);
}

#include "xhir_escape_constraints.inc.c"

static int restriction_point_compare(const void *left, const void *right) {
    const XirRestrictionPoint *a = left, *b = right;
    if (a->function != b->function) return a->function < b->function ? -1 : 1;
    if (a->instruction != b->instruction) return a->instruction < b->instruction ? -1 : 1;
    if (a->value != b->value) return a->value < b->value ? -1 : 1;
    return 0;
}

static bool restriction_publish(HirRestrictionSolver *solver, XrHirEscapeFacts **output) {
    HirEscape *owner = &solver->builder->graph;
    uint32_t functions = solver->view.function_count, total = solver->offsets[functions];
    XrHirEscapeFacts *facts = xir_compile_calloc(owner->memory, 1, sizeof(*facts), &owner->status);
    if (!facts) return false;
    XirRestrictionFunction *rows = xir_compile_calloc(owner->memory, functions, sizeof(*rows), &owner->status);
    XirRestrictionFormulaRow *formulas = xir_compile_calloc(owner->memory, total, sizeof(*formulas), &owner->status);
    uint64_t terms = 0;
    for (uint32_t n = 0; n < total; ++n) terms += solver->nodes[n].constant ? 0 : solver->nodes[n].count;
    if (terms > UINT32_MAX) owner->status = XR_XIR_OVERFLOW;
    uint32_t *inputs = owner->status == XR_XIR_OK ? xir_compile_calloc(owner->memory,
        (size_t)terms, sizeof(*inputs), &owner->status) : NULL;
    XirRestrictionPoint *points = xir_compile_calloc(owner->memory, solver->builder->observation_count,
        sizeof(*points), &owner->status);
    facts->view = (XirRestrictionView){rows, functions, formulas, total, inputs, (uint32_t)terms,
        points, solver->builder->observation_count};
    uint32_t cursor = 0;
    for (uint32_t n = 0; n < total && escape_poll(owner); ++n) {
        HirRestrictionNode node = solver->nodes[n];
        uint32_t count = node.constant ? 0 : node.count;
        formulas[n] = (XirRestrictionFormulaRow){cursor, count, node.constant};
        if (count) memcpy(inputs + cursor, node.inputs, (size_t)count * sizeof(*inputs));
        cursor += count;
    }
    for (uint32_t f = 0; f < functions && escape_poll(owner); ++f) {
        uint32_t count = solver->counts[f], flow_count = 0;
        uint32_t first = solver->offsets[f], end = solver->offsets[f + 1];
        for (uint32_t n = first; n < end && escape_poll(owner); ++n)
            for (uint32_t e = solver->nodes[n].head; e != UINT32_MAX; e = solver->edges[e].next) {
                HirRestrictionEdge edge = solver->edges[e];
                if (!edge.dependency && edge.application == UINT32_MAX &&
                    edge.target >= first && edge.target < end) ++flow_count;
            }
        uint32_t *intrinsic = xir_compile_calloc(owner->memory, count, sizeof(*intrinsic), &owner->status);
        XrXirValueFlow *flows = xir_compile_calloc(owner->memory, flow_count, sizeof(*flows), &owner->status);
        rows[f] = (XirRestrictionFunction){owner->functions[f].physical_values, count, intrinsic,
            flows, flow_count};
        for (uint32_t v = 0; v < count && escape_poll(owner); ++v) {
            intrinsic[v] = first + v;
        }
        uint32_t at = 0;
        for (uint32_t n = first; n < end && escape_poll(owner); ++n)
            for (uint32_t e = solver->nodes[n].head; e != UINT32_MAX && escape_poll(owner); e = solver->edges[e].next) {
                HirRestrictionEdge edge = solver->edges[e];
                if (!edge.dependency && edge.application == UINT32_MAX && edge.target >= first && edge.target < end)
                    flows[at++] = (XrXirValueFlow){edge.source - first, edge.target - first};
            }
    }
    for (uint32_t p = 0; p < solver->builder->observation_count && escape_poll(owner); ++p) {
        HirEscapeObservation point = solver->builder->observations[p];
        uint32_t carrier = solver->point_values[p];
        points[p] = (XirRestrictionPoint){point.function, point.instruction, point.value,
            solver->offsets[point.function] + carrier, carrier};
    }
    if (owner->status == XR_XIR_OK && solver->builder->observation_count)
        qsort(points, solver->builder->observation_count, sizeof(*points), restriction_point_compare);
    if (owner->status != XR_XIR_OK) {xr_hir_escape_facts_free(facts); return false;}
    *output = facts;
    return true;
}

XR_FUNC XrXirStatus xr_hir_escape_builder_prepare_points(XrHirEscapeBuilder *builder, XirStorageFlow *storage,
    const XirExecutionOwner *execution, bool final, bool *ready) {
    if (!builder || builder->finished || !storage || !execution || !ready) return XR_XIR_BAD_STRUCTURE;
    HirEscape *owner = &builder->graph;
    for (uint32_t f = 0; f < builder->module.function_count && escape_poll(owner); ++f)
        if (builder->observed[f] != owner->functions[f].physical_values - owner->functions[f].parameters)
            owner->status = XR_XIR_BAD_STRUCTURE;
    if (owner->status != XR_XIR_OK) return owner->status;
    HirRestrictionSolver *solver = builder->point_preparation;
    if (!solver) {
        solver = xir_compile_calloc(owner->memory, 1, sizeof(*solver), &owner->status);
        if (!solver) return owner->status;
        *solver = (HirRestrictionSolver){.builder=builder,.storage=storage,.execution=execution,
            .base_observation_count=builder->observation_count,.base_use_count=owner->use_count};
        builder->point_preparation = solver;
        solver->base_observations = xir_compile_copy(owner->memory, builder->observations,
            (size_t)builder->observation_count * sizeof(*builder->observations), &owner->status);
        if (owner->status != XR_XIR_OK) return owner->status;
    }
    if (solver->storage != storage || solver->execution != execution || (final && !solver->ready))
        return XR_XIR_BAD_STRUCTURE;
    if (solver->final) {*ready = true; return XR_XIR_OK;}
    owner->use_count = solver->base_use_count;
    builder->observation_count = solver->base_observation_count;
    if (builder->observation_count) memcpy(builder->observations, solver->base_observations,
        (size_t)builder->observation_count * sizeof(*builder->observations));
    solver->carrier_count = 0; solver->check_count = 0; solver->remainder = 0;
    bool complete = restriction_observe_storage(solver, final);
    solver->view = (XirStorageFlowView){0};
    if (!complete && owner->status == XR_XIR_UNRESOLVED && solver->remainder == XIR_EXECUTION_REMAINDER_STORAGE) {
        owner->status = XR_XIR_OK; solver->ready = false; *ready = false;
        return XR_XIR_OK;
    }
    if (owner->status != XR_XIR_OK) return owner->status;
    solver->ready = true; solver->final = final; *ready = true;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xr_hir_escape_builder_finish(XrHirEscapeBuilder *builder, XirStorageFlow *storage,
    const XirExecutionOwner *execution, XrHirEscapeDiagnostic *diagnostic, XrHirEscapeFacts **output) {
    if (!builder || builder->finished || !storage || !execution || !diagnostic || !output || *output ||
        !builder->point_preparation || !builder->point_preparation->final ||
        builder->point_preparation->storage != storage || builder->point_preparation->execution != execution)
        return XR_XIR_BAD_STRUCTURE;
    builder->finished = true;
    *diagnostic = (XrHirEscapeDiagnostic){0};
    HirEscape *owner = &builder->graph;
    HirRestrictionSolver *solver = builder->point_preparation;
    owner->status = xir_storage_flow_view(storage, &solver->view);
    if (owner->status == XR_XIR_OK && restriction_allocate(solver) && restriction_seed(solver) &&
        restriction_applications(solver) && restriction_close(solver)) {
        owner->status = restriction_constraints(solver, diagnostic);
        if (owner->status == XR_XIR_OK) restriction_publish(solver, output);
    }
    escape_points_free(builder);
    return owner->status;
}
