/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_escape_constraints.inc.c - Publication and storage extent obligations
 *
 * KEY CONCEPT:
 *   Restriction and exposure remain separate positive formulas. Applications
 *   substitute their conjunctions and the destination's symbolic frame owner.
 */
typedef struct HirStorageExtent {
    uint32_t *owners, count, capacity, head;
    bool local, external, incoming, queued;
} HirStorageExtent;
typedef struct HirStorageExtentEdge { uint32_t target, next; } HirStorageExtentEdge;
typedef enum HirLifetimeClauseKind { HIR_LIFETIME_PUBLICATION, HIR_LIFETIME_SCOPE } HirLifetimeClauseKind;
typedef struct HirLifetimeClause {
    uint32_t function, restriction, exposure_input, storage_owner, parameter, borrowed, use;
    HirLifetimeClauseKind kind;
} HirLifetimeClause;
typedef struct HirLifetimeConstraints {
    HirRestrictionSolver *solver;
    HirStorageExtent *extents;
    HirStorageExtentEdge *edges;
    HirLifetimeClause *clauses;
    uint32_t *queue, *index, *applications, *next_application;
    uint32_t count, capacity, index_capacity, edge_count, edge_capacity;
    uint32_t queue_first, queue_count;
    uint32_t failure;
} HirLifetimeConstraints;

static void lifetime_constraints_free(HirLifetimeConstraints *run) {
    uint32_t values = run->solver->offsets[run->solver->view.function_count];
    for (uint32_t v = 0; run->extents && v < values; ++v) xr_compile_resources_free(run->extents[v].owners);
    xr_compile_resources_free(run->extents); xr_compile_resources_free(run->edges);
    xr_compile_resources_free(run->clauses); xr_compile_resources_free(run->index);
    xr_compile_resources_free(run->queue); xr_compile_resources_free(run->applications);
    xr_compile_resources_free(run->next_application);
}

static void lifetime_extent_enqueue(HirLifetimeConstraints *run, uint32_t value) {
    if (run->extents[value].queued) return;
    uint32_t count = run->solver->offsets[run->solver->view.function_count];
    run->queue[(run->queue_first + run->queue_count) % count] = value;
    ++run->queue_count; run->extents[value].queued = true;
}

static bool lifetime_extent_owner(HirLifetimeConstraints *run, uint32_t value, uint32_t owner) {
    HirEscape *graph = &run->solver->builder->graph;
    HirStorageExtent *extent = &run->extents[value];
    uint32_t first = 0, end = extent->count;
    while (first < end) {
        uint32_t middle = first + (end - first) / 2;
        if (extent->owners[middle] < owner) first = middle + 1;
        else end = middle;
    }
    if (first < extent->count && extent->owners[first] == owner) return false;
    if (extent->count == extent->capacity) {
        uint32_t *grown = xir_compile_array_grow(graph->memory, extent->owners,
            extent->count, sizeof(*grown), &extent->capacity, &graph->status);
        if (!grown) return false;
        extent->owners = grown;
    }
    memmove(extent->owners + first + 1, extent->owners + first, (size_t)(extent->count - first) * sizeof(*extent->owners));
    extent->owners[first] = owner; ++extent->count;
    return true;
}

static bool lifetime_extents(HirLifetimeConstraints *run) {
    HirRestrictionSolver *solver = run->solver;
    HirEscape *graph = &solver->builder->graph;
    uint32_t functions = solver->view.function_count, values = solver->offsets[functions];
    run->extents = xir_compile_calloc(graph->memory, values, sizeof(*run->extents), &graph->status);
    run->queue = xir_compile_calloc(graph->memory, values, sizeof(*run->queue), &graph->status);
    run->applications = xir_compile_calloc(graph->memory, functions, sizeof(*run->applications), &graph->status);
    run->next_application = xir_compile_calloc(graph->memory, solver->view.application_count,
        sizeof(*run->next_application), &graph->status);
    if (graph->status != XR_XIR_OK) return false;
    for (uint32_t v = 0; v < values; ++v) run->extents[v].head = UINT32_MAX;
    for (uint32_t f = 0; f < functions; ++f) run->applications[f] = UINT32_MAX;
    for (uint32_t a = 0; a < solver->view.application_count; ++a) {
        XirStorageApplication app = solver->view.applications[a];
        run->next_application[a] = run->applications[app.declaration];
        run->applications[app.declaration] = a;
    }
    for (uint32_t e = 0; e < solver->view.identity_count && escape_poll(graph); ++e) {
        XirStorageIdentity identity = solver->view.identities[e];
        uint32_t from = restriction_node(solver, identity.source_function, identity.source, false);
        uint32_t to = restriction_node(solver, identity.target_function, identity.target, false);
        if (graph->status != XR_XIR_OK) return false;
        run->extents[to].incoming = true;
        if (identity.application != UINT32_MAX || identity.source_function != identity.target_function) continue;
        if (run->edge_count == run->edge_capacity) {
            HirStorageExtentEdge *grown = xir_compile_array_grow(graph->memory, run->edges,
                run->edge_count, sizeof(*grown), &run->edge_capacity, &graph->status);
            if (!grown) return false;
            run->edges = grown;
        }
        uint32_t index = run->edge_count++;
        run->edges[index] = (HirStorageExtentEdge){to, run->extents[from].head};
        run->extents[from].head = index;
    }
    for (uint32_t f = 0; f < functions && escape_poll(graph); ++f) {
        HirEscapeFunction function = graph->functions[f];
        for (uint32_t v = 0; v < solver->counts[f]; ++v) {
            uint32_t index = solver->offsets[f] + v;
            HirStorageExtent *extent = &run->extents[index];
            if (solver->nodes[index].allocated) extent->local = true;
            else if (v < function.captures && xr_xir_type_is_cell(graph->module->types,
                graph->nodes[graph->offsets[f] + v].type)) lifetime_extent_owner(run, index, v);
            else if (v < function.physical_values && !extent->incoming) extent->external = true;
            if (extent->local || extent->external || extent->count) lifetime_extent_enqueue(run, index);
        }
    }
    while (run->queue_count && escape_poll(graph)) {
        uint32_t source = run->queue[run->queue_first];
        run->queue_first = (run->queue_first + 1) % values;
        --run->queue_count; run->extents[source].queued = false;
        for (uint32_t e = run->extents[source].head; e != UINT32_MAX && escape_poll(graph); e = run->edges[e].next) {
            uint32_t target = run->edges[e].target;
            HirStorageExtent *to = &run->extents[target], *from = &run->extents[source];
            bool changed = (from->local && !to->local) || (from->external && !to->external);
            to->local |= from->local; to->external |= from->external;
            for (uint32_t p = 0; p < from->count; ++p) changed |= lifetime_extent_owner(run, target, from->owners[p]);
            if (changed) lifetime_extent_enqueue(run, target);
        }
    }
    return graph->status == XR_XIR_OK;
}

static uint32_t lifetime_clause_hash(HirLifetimeClause clause) {
    const uint32_t fields[] = {clause.function, clause.restriction, clause.exposure_input,
        clause.storage_owner, (uint32_t)clause.kind, clause.parameter, clause.borrowed};
    return xr_hash_bytes(fields, sizeof(fields));
}

static bool lifetime_clause_equal(HirLifetimeClause a, HirLifetimeClause b) {
    return a.function == b.function && a.restriction == b.restriction && a.exposure_input == b.exposure_input && a.storage_owner == b.storage_owner &&
        a.kind == b.kind && a.parameter == b.parameter && a.borrowed == b.borrowed;
}

static bool lifetime_clause_add(HirLifetimeConstraints *run, HirLifetimeClause clause) {
    HirEscape *graph = &run->solver->builder->graph;
    if (!run->index_capacity || run->count >= run->index_capacity / 2) {
        uint64_t next = run->index_capacity ? (uint64_t)run->index_capacity * 2 : 32;
        if (next > UINT32_MAX) {graph->status = XR_XIR_OVERFLOW; return false;}
        uint32_t *index = xir_compile_calloc(graph->memory, (size_t)next, sizeof(*index), &graph->status);
        if (!index) return false;
        for (uint32_t c = 0; c < run->count && escape_poll(graph); ++c) {
            uint32_t slot = lifetime_clause_hash(run->clauses[c]) & ((uint32_t)next - 1);
            while (index[slot] && escape_poll(graph)) slot = (slot + 1) & ((uint32_t)next - 1);
            index[slot] = c + 1;
        }
        if (graph->status != XR_XIR_OK) {xr_compile_resources_free(index); return false;}
        xr_compile_resources_free(run->index); run->index = index; run->index_capacity = (uint32_t)next;
    }
    uint32_t slot = lifetime_clause_hash(clause) & (run->index_capacity - 1);
    while (run->index[slot]) {
        if (!escape_poll(graph)) return false;
        if (lifetime_clause_equal(run->clauses[run->index[slot] - 1], clause)) return true;
        slot = (slot + 1) & (run->index_capacity - 1);
    }
    if (run->count == run->capacity) {
        HirLifetimeClause *grown = xir_compile_array_grow(graph->memory, run->clauses,
            run->count, sizeof(*grown), &run->capacity, &graph->status);
        if (!grown) return false;
        run->clauses = grown;
    }
    run->index[slot] = run->count + 1;
    if (run->failure == UINT32_MAX && clause.kind == HIR_LIFETIME_PUBLICATION &&
        clause.restriction == UINT32_MAX && clause.exposure_input == UINT32_MAX) run->failure = run->count;
    run->clauses[run->count++] = clause;
    return true;
}

static bool lifetime_scope(HirLifetimeConstraints *run, HirLifetimeClause clause, uint32_t handle) {
    HirRestrictionSolver *solver = run->solver;
    HirEscape *graph = &solver->builder->graph;
    uint32_t index = restriction_node(solver, clause.function, handle, false);
    if (graph->status != XR_XIR_OK) return false;
    HirStorageExtent extent = run->extents[index];
    if (!extent.local && !extent.external && !extent.count) {graph->status = XR_XIR_UNRESOLVED; return false;}
    if (extent.external) {
        HirLifetimeClause publication = clause;
        publication.kind = HIR_LIFETIME_PUBLICATION; publication.exposure_input = UINT32_MAX;
        publication.storage_owner = UINT32_MAX;
        if (!lifetime_clause_add(run, publication)) return false;
    }
    for (uint32_t p = 0; p < extent.count && escape_poll(graph); ++p) {
        HirLifetimeClause scoped = clause;
        scoped.kind = HIR_LIFETIME_SCOPE; scoped.storage_owner = extent.owners[p];
        scoped.exposure_input = UINT32_MAX;
        if (scoped.restriction == UINT32_MAX) {
            uint32_t origin = scoped.parameter ? scoped.parameter : scoped.borrowed;
            if (!origin || origin - 1 < graph->offsets[clause.function] ||
                origin - 1 >= graph->offsets[clause.function + 1]) {graph->status = XR_XIR_UNRESOLVED; return false;}
            scoped.kind = HIR_LIFETIME_PUBLICATION; scoped.storage_owner = UINT32_MAX;
        }
        if (!lifetime_clause_add(run, scoped)) return false;
    }
    return graph->status == XR_XIR_OK;
}

static bool lifetime_seed(HirLifetimeConstraints *run) {
    HirRestrictionSolver *solver = run->solver;
    HirEscape *graph = &solver->builder->graph;
    for (uint32_t c = 0; c < solver->check_count && run->failure == UINT32_MAX && escape_poll(graph); ++c) {
        HirRestrictionCheck *check = &solver->checks[c];
        HirEscapeUse use = graph->uses[check->use];
        check->carrier = restriction_use_carrier(solver, use);
        if (graph->status != XR_XIR_OK) return false;
        HirRestrictionNode r = solver->nodes[check->carrier];
        if (restriction_empty(&r)) continue;
        HirRestrictionNode x = {.constant=true};
        if (use.storage != UINT32_MAX) {
            uint32_t exposure = restriction_node(solver, use.function, check->exposure, true);
            if (graph->status != XR_XIR_OK) return false;
            x = solver->nodes[exposure];
        }
        for (uint32_t p = 0; p < (r.constant ? 1u : r.count) && escape_poll(graph); ++p) {
            HirLifetimeClause clause = {.function=use.function, .restriction=r.constant ? UINT32_MAX : r.inputs[p],
                .exposure_input=UINT32_MAX, .storage_owner=UINT32_MAX, .parameter=r.constant ? r.parameter : 0,
                .borrowed=r.constant ? r.borrowed : 0, .use=check->use, .kind=HIR_LIFETIME_PUBLICATION};
            if (use.storage != UINT32_MAX && !lifetime_scope(run, clause, use.storage - graph->offsets[use.function])) return false;
            for (uint32_t q = 0; q < (x.constant ? 1u : x.count); ++q) {
                clause.exposure_input = x.constant ? UINT32_MAX : x.inputs[q];
                if (!lifetime_clause_add(run, clause)) return false;
            }
        }
    }
    return graph->status == XR_XIR_OK;
}

static bool lifetime_substitute_pair(HirLifetimeConstraints *run, HirLifetimeClause clause,
    XirStorageApplication app, const HirRestrictionNode *r, const HirRestrictionNode *x) {
    HirEscape *graph = &run->solver->builder->graph;
    for (uint32_t p = 0; p < (r->constant ? 1u : r->count) && escape_poll(graph); ++p)
        for (uint32_t q = 0; q < (x->constant ? 1u : x->count) && escape_poll(graph); ++q) {
            HirLifetimeClause target = {.function=app.function, .restriction=r->constant ? UINT32_MAX : r->inputs[p],
                .exposure_input=x->constant ? UINT32_MAX : x->inputs[q], .storage_owner=UINT32_MAX,
                .parameter=r->constant ? r->parameter : 0, .borrowed=r->constant ? r->borrowed : 0,
                .use=clause.use, .kind=HIR_LIFETIME_PUBLICATION};
            if (!lifetime_clause_add(run, target)) return false;
        }
    return graph->status == XR_XIR_OK;
}

static bool lifetime_actual(HirLifetimeConstraints *run, XirStorageApplication app,
    XirStorageBinding binding, bool exposure, HirRestrictionNode *output) {
    HirRestrictionSolver *solver = run->solver;
    HirEscape *graph = &solver->builder->graph;
    uint32_t index = restriction_node(solver, binding.actual_function, binding.actual_value, exposure);
    if (graph->status != XR_XIR_OK) return false;
    *output = solver->nodes[index];
    if (!restriction_empty(output) && !output->constant && (binding.actual_function != app.function ||
        binding.actual_environment != XIR_EXECUTION_CURRENT_ENVIRONMENT)) {
        graph->status = XR_XIR_UNRESOLVED; return false;
    }
    return true;
}

static bool lifetime_apply(HirLifetimeConstraints *run, HirLifetimeClause clause, XirStorageApplication app) {
    HirRestrictionSolver *solver = run->solver;
    HirEscape *graph = &solver->builder->graph;
    bool found_r = false;
    for (uint32_t p = 0; p < (clause.restriction == UINT32_MAX ? 1u : app.count) && escape_poll(graph); ++p) {
        HirRestrictionNode r = {.constant=true,.parameter=clause.parameter,.borrowed=clause.borrowed};
        if (clause.restriction != UINT32_MAX) {
            XirStorageBinding binding = solver->view.bindings[app.first + p];
            if (binding.formal_function != clause.function || binding.formal_value != clause.restriction) continue;
            if (!lifetime_actual(run, app, binding, false, &r)) return false;
        }
        found_r = true;
        if (restriction_empty(&r)) continue;
        uint32_t required = clause.kind == HIR_LIFETIME_SCOPE ? clause.storage_owner : clause.exposure_input;
        bool found_other = false;
        for (uint32_t q = 0; q < (required == UINT32_MAX ? 1u : app.count) && escape_poll(graph); ++q) {
            HirRestrictionNode x = {.constant=true};
            uint32_t handle = UINT32_MAX;
            if (required != UINT32_MAX) {
                XirStorageBinding binding = solver->view.bindings[app.first + q];
                if (binding.formal_function != clause.function || binding.formal_value != required) continue;
                if (clause.kind == HIR_LIFETIME_SCOPE) {
                    if (binding.actual_function != app.function || binding.actual_environment != XIR_EXECUTION_CURRENT_ENVIRONMENT) {
                        graph->status = XR_XIR_UNRESOLVED; return false;
                    }
                    handle = binding.actual_value;
                } else if (!lifetime_actual(run, app, binding, true, &x)) return false;
            }
            found_other = true;
            if (clause.kind == HIR_LIFETIME_SCOPE) {
                for (uint32_t term = 0; term < (r.constant ? 1u : r.count) && escape_poll(graph); ++term) {
                    HirLifetimeClause scoped = {.function=app.function, .restriction=r.constant ? UINT32_MAX : r.inputs[term],
                        .exposure_input=UINT32_MAX, .storage_owner=handle, .parameter=r.constant ? r.parameter : 0,
                        .borrowed=r.constant ? r.borrowed : 0, .use=clause.use, .kind=HIR_LIFETIME_SCOPE};
                    if (!lifetime_scope(run, scoped, handle)) return false;
                }
            } else if (!lifetime_substitute_pair(run, clause, app, &r, &x)) return false;
        }
        if (!found_other && !restriction_empty(&r)) {graph->status = XR_XIR_UNRESOLVED; return false;}
    }
    if (!found_r) {graph->status = XR_XIR_UNRESOLVED; return false;}
    return graph->status == XR_XIR_OK;
}

static XrXirStatus restriction_constraints(HirRestrictionSolver *solver, XrHirEscapeDiagnostic *diagnostic) {
    HirEscape *graph = &solver->builder->graph;
    HirLifetimeConstraints run = {.solver=solver,.failure=UINT32_MAX};
    if (lifetime_extents(&run) && lifetime_seed(&run))
        for (uint32_t c = 0; c < run.count && run.failure == UINT32_MAX && escape_poll(graph); ++c) {
            HirLifetimeClause clause = run.clauses[c];
            for (uint32_t a = run.applications[clause.function]; a != UINT32_MAX && escape_poll(graph); a = run.next_application[a])
                if (!lifetime_apply(&run, clause, solver->view.applications[a])) break;
        }
    if (graph->status == XR_XIR_OK && run.failure != UINT32_MAX) {
        HirLifetimeClause clause = run.clauses[run.failure];
        HirEscapeUse use = graph->uses[clause.use];
        graph->status = clause.parameter || clause.borrowed ? escape_reject_origin(graph, use.function, use.instruction,
            (HirEscapeOrigin){clause.parameter, clause.borrowed}, use.reason, diagnostic) : XR_XIR_BAD_STRUCTURE;
    }
    lifetime_constraints_free(&run);
    return graph->status;
}
