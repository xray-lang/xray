/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_execution_origins.inc.c - Contextual callable origins from owned flows
 */
static void execution_origins_clear(EffectCheckOrigins *origins) {
    xr_compile_resources_free(origins->offsets);
    xr_compile_resources_free(origins->heads);
    xr_compile_resources_free(origins->edges);
    *origins = (EffectCheckOrigins){0};
}

#include "xxir_execution_results.inc.c"

/* One receiving index serves all call queries in a linking round. Neither a
 * query nor a graph walk scans complete per-function flow arrays. */
static XrXirStatus execution_origins_receive(const XrXirCompileContext *memory,
    EffectCheckedRoots *roots, const XirStorageFlowView *view) {
    if (view->function_count != roots->templates) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    EffectCheckOrigins next = {0};
    next.offsets = xir_compile_alloc(memory, ((size_t)roots->templates + 1) * sizeof(*next.offsets), &status);
    if (!next.offsets) return status;
    uint64_t nodes = 0, edges = 0;
    for (uint32_t f = 0; f < roots->templates; ++f) {
        next.offsets[f] = (uint32_t)nodes;
        nodes += view->functions[f].value_count;
        edges += view->functions[f].flow_count;
        if (nodes > UINT32_MAX || edges > UINT32_MAX) {status = XR_XIR_OVERFLOW; goto done;}
    }
    next.offsets[roots->templates] = next.nodes = (uint32_t)nodes;
    for (uint32_t e = 0; e < view->crossflow_count; ++e)
        if (view->crossflows[e].bank == XIR_STORAGE_CONTENTS && ++edges > UINT32_MAX) {
            status = XR_XIR_OVERFLOW; goto done;
        }
    for (uint32_t i = 0; i < view->input_count; ++i)
        if (view->inputs[i].bank == XIR_STORAGE_CONTENTS && ++edges > UINT32_MAX) {
            status = XR_XIR_OVERFLOW; goto done;
        }
    uint32_t results = 0;
    for (uint32_t c = 0; c < roots->calls; ++c)
        if (execution_result_call(roots,roots->call[c])) ++results;
    for (uint32_t u = 0; u < view->unknown_count; ++u)
        if (view->unknowns[u].bank == XIR_STORAGE_CONTENTS && ++edges > UINT32_MAX) {
            status = XR_XIR_OVERFLOW; goto done;
        }
    edges += (uint64_t)results + view->bottom_count;
    if (edges > UINT32_MAX) {status = XR_XIR_OVERFLOW; goto done;}
    if (nodes > SIZE_MAX / sizeof(*next.heads) || edges > SIZE_MAX / sizeof(*next.edges)) {
        status = XR_XIR_OVERFLOW; goto done;
    }
    next.heads = nodes ? xir_compile_alloc(memory, (size_t)nodes * sizeof(*next.heads), &status) : NULL;
    next.edges = edges ? xir_compile_alloc(memory, (size_t)edges * sizeof(*next.edges), &status) : NULL;
    if (status != XR_XIR_OK) goto done;
    if (nodes) memset(next.heads, 255, (size_t)nodes * sizeof(*next.heads));
    for (uint32_t f = 0; f < roots->templates; ++f) {
        XirStorageFlowFunction function = view->functions[f];
        if (function.physical_values != roots->function[f].values) {status = XR_XIR_BAD_STRUCTURE; goto done;}
        for (uint32_t e = 0; e < function.flow_count; ++e) {
            XrXirValueFlow flow = function.flows[e];
            if (flow.source >= function.value_count || flow.target >= function.value_count) {
                status = XR_XIR_BAD_STRUCTURE; goto done;
            }
            uint32_t target = next.offsets[f] + flow.target;
            next.edges[next.count] = (EffectCheckOriginEdge){f, flow.source, UINT32_MAX, next.heads[target], false, false, false};
            next.heads[target] = next.count++;
        }
    }
    for (uint32_t e = 0; e < view->crossflow_count; ++e) {
        XirStorageCrossFlow flow = view->crossflows[e];
        if (flow.bank != XIR_STORAGE_CONTENTS) continue;
        if (flow.source_function >= roots->templates || flow.target_function >= roots->templates ||
            flow.source >= view->functions[flow.source_function].value_count ||
            flow.target >= view->functions[flow.target_function].value_count ||
            (flow.application != UINT32_MAX && flow.application >= view->application_count)) {
            status = XR_XIR_BAD_STRUCTURE; goto done;
        }
        uint32_t target = next.offsets[flow.target_function] + flow.target;
        next.edges[next.count] = (EffectCheckOriginEdge){flow.source_function, flow.source,
            flow.application, next.heads[target], false, false, false};
        next.heads[target] = next.count++;
    }
    for (uint32_t i = 0; i < view->input_count; ++i) {
        XirStorageInput input = view->inputs[i];
        if (input.bank != XIR_STORAGE_CONTENTS) continue;
        if (input.function >= roots->templates ||
            input.value >= view->functions[input.function].value_count ||
            input.handle >= roots->function[input.function].parameters ||
            !roots->node[roots->function[input.function].first + input.handle].cell) {
            status = XR_XIR_BAD_STRUCTURE; goto done;
        }
        uint32_t target = next.offsets[input.function] + input.value;
        next.edges[next.count] = (EffectCheckOriginEdge){input.function, input.handle,
            UINT32_MAX, next.heads[target], true, false, false};
        next.heads[target] = next.count++;
    }
    for (uint32_t u = 0; u < view->unknown_count; ++u) {
        XirStorageUnknown unknown = view->unknowns[u];
        if ((uint32_t)unknown.bank > XIR_STORAGE_FOOTPRINT || unknown.function >= roots->templates ||
            unknown.value >= view->functions[unknown.function].value_count ||
            (unknown.application != UINT32_MAX && unknown.application >= view->application_count)) {
            status = XR_XIR_BAD_STRUCTURE; goto done;
        }
        if (unknown.bank != XIR_STORAGE_CONTENTS) continue;
        uint32_t target = next.offsets[unknown.function] + unknown.value;
        /* An entry's typed upper bound is represented by its formal input.
         * Later opaque writes have different version nodes and remain opaque. */
        if (unknown.application == UINT32_MAX && next.heads[target] != UINT32_MAX &&
            next.edges[next.heads[target]].parameter_contents) continue;
        next.edges[next.count] = (EffectCheckOriginEdge){UINT32_MAX, 0, unknown.application, next.heads[target], false, false, false};
        next.heads[target] = next.count++;
    }
    for (uint32_t b = 0; b < view->bottom_count; ++b) {
        XirStorageBottom bottom = view->bottoms[b];
        if (bottom.function >= roots->templates ||
            bottom.value < view->functions[bottom.function].physical_values ||
            bottom.value >= view->functions[bottom.function].value_count) {status=XR_XIR_BAD_STRUCTURE;goto done;}
        uint32_t target = next.offsets[bottom.function] + bottom.value;
        next.edges[next.count] = (EffectCheckOriginEdge){UINT32_MAX,0,UINT32_MAX,next.heads[target],false,false,true};
        next.heads[target] = next.count++;
    }
    if (results) status = execution_result_edges(memory,roots,view,&next);
    if (status != XR_XIR_OK) goto done;
    execution_origins_clear(&roots->storage_origins);
    roots->storage_origins = next;
    return XR_XIR_OK;
done:
    execution_origins_clear(&next);
    return status;
}

typedef struct ExecutionOrigin {
    uint32_t function, value, environment;
    bool equation;
    uint32_t entry_context;
    bool storable;
} ExecutionOrigin;

typedef struct ExecutionEntryIndex {
    uint32_t *incoming, *next_application;
    bool indexed;
} ExecutionEntryIndex;
typedef struct ExecutionOriginWalk {
    XrXirCompileContext memory;
    ExecutionOrigin *items;
    uint32_t *index;
    uint32_t count, capacity, index_capacity;
    ExecutionEntryIndex *entries;
    XirExecutionVisitor visit;
    void *context;
    bool observe_environment, footprint;
    XrXirStatus status;
} ExecutionOriginWalk;

static uint32_t execution_origin_hash(ExecutionOrigin origin) {
    return xr_hash_int(origin.function) ^ xr_hash_int(origin.value) * 3u ^ xr_hash_int(origin.environment) * 7u ^
        (origin.equation ? UINT32_C(0x9e3779b9) : 0) ^ xr_hash_int(origin.entry_context) * 11u ^
        (origin.storable ? UINT32_C(0x85ebca6b) : 0);
}

static bool execution_origin_push(ExecutionOriginWalk *walk, ExecutionOrigin origin) {
    if (walk->status != XR_XIR_OK) return false;
    if (!walk->index_capacity || walk->count >= walk->index_capacity / 2) {
        uint64_t capacity = walk->index_capacity ? (uint64_t)walk->index_capacity * 2 : 32;
        if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof(uint32_t)) {
            walk->status = XR_XIR_OVERFLOW; return false;
        }
        uint32_t *index = xir_compile_calloc(&walk->memory, (size_t)capacity, sizeof(*index), &walk->status);
        if (!index) return false;
        for (uint32_t n = 0; n < walk->count; ++n) {
            uint32_t at = execution_origin_hash(walk->items[n]) & ((uint32_t)capacity - 1);
            while (index[at]) at = (at + 1) & ((uint32_t)capacity - 1);
            index[at] = n + 1;
        }
        xr_compile_resources_free(walk->index);
        walk->index = index;
        walk->index_capacity = (uint32_t)capacity;
    }
    uint32_t at = execution_origin_hash(origin) & (walk->index_capacity - 1);
    while (walk->index[at]) {
        ExecutionOrigin found = walk->items[walk->index[at] - 1];
        if (found.function == origin.function && found.value == origin.value &&
            found.environment == origin.environment && found.equation == origin.equation &&
            found.entry_context == origin.entry_context && found.storable == origin.storable) return true;
        at = (at + 1) & (walk->index_capacity - 1);
    }
    if (walk->count == walk->capacity) {
        ExecutionOrigin *items = xir_compile_array_grow(&walk->memory, walk->items,
            walk->count, sizeof(*items), &walk->capacity, &walk->status);
        if (!items) return false;
        walk->items = items;
    }
    walk->items[walk->count] = origin;
    walk->index[at] = ++walk->count;
    return true;
}

static XirExecutionSite execution_resolved_site(const EffectCheckedRoots *roots,
    const XirExecutionSite *pending, uint32_t reference, uint32_t environment) {
    const EffectCheckCall *capture = &roots->call[reference];
    XirExecutionSite site = *pending;
    site.reference = reference;
    site.capture_environment = environment;
    site.target = roots->node[capture->target].function;
    site.declaration = roots->function[site.target].declaration;
    site.capture_count = roots->function[site.target].capture_count;
    site.native_callable = roots->function[site.target].native_callable;
    site.argument_count += capture->count;
    site.remainder = 0;
    site.unresolved = false;
    return site;
}

typedef enum ExecutionOriginQuery {
    EXECUTION_ORIGINS_CALLABLE, EXECUTION_ORIGINS_EFFECTS,
    EXECUTION_ORIGINS_ENVIRONMENT, EXECUTION_ORIGINS_ENVIRONMENT_INPUTS, EXECUTION_ORIGINS_CALLBACK_ENTRY,
    EXECUTION_ORIGINS_FOOTPRINT
} ExecutionOriginQuery;

typedef struct ExecutionOriginRequest {
    ExecutionOriginQuery kind;
    ExecutionEntryIndex *entries;
} ExecutionOriginRequest;

static bool execution_origin_incoming(ExecutionOriginWalk *walk, const EffectCheckedRoots *roots,
    const XirStorageFlowView *storage, ExecutionOrigin origin) {
    ExecutionEntryIndex *entries = walk->entries;
    if (!entries) {walk->status = XR_XIR_BAD_STRUCTURE; return false;}
    /* All receivers in one observation share this reverse index. Existing
     * execution applications supply its finite frame identities. */
    if (!entries->indexed) {
        entries->incoming = xir_compile_calloc(&walk->memory, roots->functions, sizeof(*entries->incoming), &walk->status);
        entries->next_application = xir_compile_calloc(&walk->memory, storage->application_count,
            sizeof(*entries->next_application), &walk->status);
        if (walk->status != XR_XIR_OK) return false;
        for (uint32_t f = 0; f < roots->functions; ++f) entries->incoming[f] = UINT32_MAX;
        for (uint32_t a = 0; a < storage->application_count; ++a) {
            XirStorageApplication app = storage->applications[a];
            if (app.kind == XIR_EXECUTION_CALLBACK_ENTRY || app.outcome != XIR_EXECUTION_NORMAL ||
                (app.protocol != 2 && app.kind != XIR_EXECUTION_CLEANUP_USE)) continue;
            if (app.target >= roots->functions) {walk->status = XR_XIR_BAD_STRUCTURE; return false;}
            entries->next_application[a] = entries->incoming[app.target]; entries->incoming[app.target] = a;
        }
        entries->indexed = true;
    }
    bool found = false;
    for (uint32_t a = entries->incoming[origin.function]; a != UINT32_MAX; a = entries->next_application[a]) {
        execution_origin_push(walk, (ExecutionOrigin){origin.function, origin.value, a, false, a, origin.storable});
        found = true;
    }
    return found;
}

static bool execution_origin_binding(ExecutionOriginWalk *walk, const EffectCheckedRoots *roots,
    const XirStorageFlowView *storage, ExecutionOrigin origin, uint32_t *unresolved) {
    if (origin.environment >= XIR_EXECUTION_UNKNOWN_ENVIRONMENT) return false;
    if (origin.environment >= storage->application_count) {walk->status = XR_XIR_BAD_STRUCTURE; return true;}
    XirStorageApplication app = storage->applications[origin.environment];
    if (app.kind == XIR_EXECUTION_CALLBACK_ENTRY) {
        *unresolved |= XIR_EXECUTION_REMAINDER_OPAQUE;
        return true;
    }
    if (origin.equation || app.declaration != roots->function[origin.function].declaration) return false;
    if (app.first > storage->binding_count || app.count > storage->binding_count - app.first) {
        walk->status = XR_XIR_BAD_STRUCTURE; return true;
    }
    bool bound = false;
    for (uint32_t b = 0; b < app.count; ++b) {
        XirStorageBinding binding = storage->bindings[app.first + b];
        if (binding.formal_function != app.declaration || binding.formal_value != origin.value) continue;
        uint32_t environment = binding.actual_environment == XIR_EXECUTION_CURRENT_ENVIRONMENT ?
            app.environment : binding.actual_environment;
        uint32_t function = binding.actual_function;
        if (app.caller_instance < roots->functions &&
            roots->function[app.caller_instance].declaration == function) function = app.caller_instance;
        if (environment == XIR_EXECUTION_UNKNOWN_ENVIRONMENT) *unresolved |= XIR_EXECUTION_REMAINDER_OPAQUE;
        else execution_origin_push(walk, (ExecutionOrigin){function, binding.actual_value,
            environment, false, origin.entry_context, origin.storable});
        bound = true;
    }
    return bound;
}

static bool execution_origin_input(ExecutionOriginWalk *walk, const EffectCheckedRoots *roots,
    const XirStorageFlowView *storage, ExecutionOrigin origin, EffectCheckOriginEdge edge, uint32_t *unresolved) {
    EffectCheckFunction function = roots->function[origin.function];
    if (edge.function != function.declaration || edge.value >= function.parameters) {
        walk->status = XR_XIR_BAD_STRUCTURE; return false;
    }
    XrXirType payload = xr_xir_cell_element(xir_execution_types(roots),roots->node[function.first+edge.value].type);
    if (walk->entries && origin.environment == XIR_EXECUTION_CURRENT_ENVIRONMENT &&
        execution_origin_incoming(walk, roots, storage, origin)) return false;
    if (walk->footprint && edge.value < function.capture_count &&
        xr_xir_type_is_callable(xir_execution_types(roots), payload)) {
        XirExecutionSite input = {.kind=XIR_EXECUTION_CONTENTS_INPUT, .index=edge.value,
            .function=function.declaration, .instruction=UINT32_MAX, .block=UINT32_MAX,
            .caller_instance=origin.function, .target=UINT32_MAX, .declaration=function.declaration,
            .argument_count=1, .capture_count=function.capture_count,
            .output_function=UINT32_MAX, .output_value=UINT32_MAX,
            .receiver_function=function.declaration, .receiver_value=origin.value,
            .registration=UINT32_MAX, .reference=UINT32_MAX, .environment=origin.environment,
            .capture_environment=origin.environment, .signature=payload};
        walk->status = walk->visit(walk->context, &input);
        return true;
    }
    if (walk->observe_environment && edge.value >= function.capture_count &&
        xr_xir_type_is_callable(xir_execution_types(roots),payload)) {
        XirExecutionSite stored = {.kind=XIR_EXECUTION_STORABLE,.index=origin.value,
            .function=function.declaration,.instruction=UINT32_MAX,.block=UINT32_MAX,
            .caller_instance=origin.function,.target=UINT32_MAX,.declaration=UINT32_MAX,
            .output_function=UINT32_MAX,.output_value=UINT32_MAX,
            .receiver_function=function.declaration,.receiver_value=origin.value,
            .registration=UINT32_MAX,.reference=UINT32_MAX,.environment=origin.environment,
            .capture_environment=origin.environment,.signature=payload};
        walk->status = walk->visit(walk->context,&stored);
        if (walk->footprint) *unresolved |= XIR_EXECUTION_REMAINDER_STORABLE_ENVIRONMENT;
        return true;
    }
    *unresolved |= origin.storable ? XIR_EXECUTION_REMAINDER_STORABLE_ENVIRONMENT : XIR_EXECUTION_REMAINDER_OPAQUE;
    return false;
}

static XrXirStatus execution_visit_origins(const EffectCheckedRoots *roots,
    uint32_t function_id, uint32_t value, uint32_t environment, XirExecutionVisitor visit, void *context,
    bool *complete, uint32_t *remainder, ExecutionOriginRequest request) {
    if (!roots || !visit || !complete || !remainder || function_id >= roots->functions)
        return XR_XIR_BAD_STRUCTURE;
    ExecutionOriginQuery query = request.kind;
    bool observe_environment = query == EXECUTION_ORIGINS_ENVIRONMENT ||
        query == EXECUTION_ORIGINS_ENVIRONMENT_INPUTS || query == EXECUTION_ORIGINS_FOOTPRINT;
    XirStorageFlowView storage = {0};
    if (roots->storage) {
        XrXirStatus status = xir_storage_flow_view(roots->storage, &storage);
        if (status != XR_XIR_OK) return status;
    }
    ExecutionOriginWalk walk = {.memory = {.resources = roots->resources},.entries=request.entries,
        .visit=visit,.context=context,.observe_environment=observe_environment,.footprint=query==EXECUTION_ORIGINS_FOOTPRINT};
    execution_origin_push(&walk, (ExecutionOrigin){function_id, value, environment, false,
        query == EXECUTION_ORIGINS_CALLBACK_ENTRY ? UINT32_MAX : 0, false});
    bool found = false;
    uint32_t unresolved = 0;
    for (uint32_t cursor = 0; cursor < walk.count && walk.status == XR_XIR_OK; ++cursor) {
        if (roots->storage) {
            walk.status = xir_storage_flow_view(roots->storage, &storage);
            if (walk.status != XR_XIR_OK) break;
        }
        ExecutionOrigin origin = walk.items[cursor];
        if (origin.function >= roots->functions) {walk.status = XR_XIR_BAD_STRUCTURE; break;}
        EffectCheckFunction function = roots->function[origin.function];
        if (execution_origin_binding(&walk, roots, &storage, origin, &unresolved)) continue;
        bool predecessor = false;
        if (origin.value < function.values || origin.equation) {
            if (origin.value > function.last - function.first) {walk.status = XR_XIR_BAD_STRUCTURE; break;}
            const EffectCheckNode *node = &roots->node[function.first + origin.value];
            if (node->callable_input) {
                if (query == EXECUTION_ORIGINS_CALLBACK_ENTRY && origin.environment == XIR_EXECUTION_CURRENT_ENVIRONMENT &&
                    execution_origin_incoming(&walk, roots, &storage, origin)) continue;
                if (observe_environment && origin.value < function.parameters &&
                    (origin.value >= function.capture_count || query == EXECUTION_ORIGINS_ENVIRONMENT_INPUTS || walk.footprint)) {
                    XirExecutionSiteKind kind = origin.value < function.capture_count ?
                        XIR_EXECUTION_CAPTURE_INPUT : XIR_EXECUTION_FORMAL;
                    XirExecutionSite formal = {.kind = kind, .index = origin.value,
                        .function = function.declaration, .instruction = UINT32_MAX, .block = UINT32_MAX,
                        .caller_instance = origin.function, .target = UINT32_MAX, .declaration = function.declaration,
                        .argument_count = 1, .capture_count = function.capture_count,
                        .output_function = UINT32_MAX, .output_value = UINT32_MAX,
                        .receiver_function = function.declaration, .receiver_value = origin.value,
                        .registration = UINT32_MAX, .reference = UINT32_MAX, .environment = origin.environment,
                        .capture_environment = origin.environment, .signature = node->type};
                    walk.status = visit(context, &formal);
                    found = true;
                } else unresolved |= origin.storable ? XIR_EXECUTION_REMAINDER_STORABLE_ENVIRONMENT :
                    XIR_EXECUTION_REMAINDER_PARAMETER;
                continue;
            }
            if (query == EXECUTION_ORIGINS_EFFECTS && node->erased && !node->restricted && !function.default_callback) {
                unresolved |= XIR_EXECUTION_REMAINDER_OPAQUE;
                continue;
            }
            if (observe_environment && node->storable_value &&
                xr_xir_type_is_callable(xir_execution_types(roots), node->type)) {
                XirExecutionSite stored = {.kind = XIR_EXECUTION_STORABLE, .index = origin.value,
                    .function = function.declaration, .instruction = origin.value >= function.parameters ?
                        origin.value - function.parameters : UINT32_MAX, .block = node->block,
                    .caller_instance = origin.function, .target = UINT32_MAX, .declaration = UINT32_MAX,
                    .output_function = UINT32_MAX, .output_value = UINT32_MAX,
                    .receiver_function = function.declaration, .receiver_value = origin.value,
                    .registration = UINT32_MAX, .reference = UINT32_MAX, .environment = origin.environment,
                    .capture_environment = origin.environment, .signature = node->type};
                walk.status = visit(context, &stored);
                found = true;
                if (walk.footprint) origin.storable = true;
                else predecessor = true;
            } else if (node->callable_opaque) unresolved |= origin.storable ?
                XIR_EXECUTION_REMAINDER_STORABLE_ENVIRONMENT : XIR_EXECUTION_REMAINDER_OPAQUE;
            for (uint32_t c = node->calls; c != UINT32_MAX && walk.status == XR_XIR_OK; c = roots->call[c].next) {
                EffectCheckCall call = roots->call[c];
                if (call.protocol != 1) continue;
                XirExecutionSite capture = execution_call_site(roots, c);
                capture.environment = origin.environment;
                capture.capture_environment = origin.environment;
                capture.entry_context = origin.entry_context;
                capture.storable_environment = walk.footprint && origin.storable;
                walk.status = visit(context, &capture);
                predecessor = found = true;
            }
            for (uint32_t a = node->aliases; a != UINT32_MAX; a = roots->alias[a].next) {
                if (!roots->alias[a].callable_origin) continue;
                uint32_t source = roots->alias[a].source;
                EffectCheckFunction source_function = roots->function[roots->node[source].function];
                uint32_t source_value = source - source_function.first;
                execution_origin_push(&walk, (ExecutionOrigin){roots->node[source].function,
                    source_value, origin.environment, source_value >= source_function.values, origin.entry_context, origin.storable});
                predecessor = true;
            }
        }
        const EffectCheckOrigins *origins = &roots->storage_origins;
        if (!origin.equation && origins->offsets && function.declaration < roots->templates &&
            origin.value < origins->offsets[function.declaration + 1] - origins->offsets[function.declaration]) {
            uint32_t index = origins->offsets[function.declaration] + origin.value;
            for (uint32_t e = origins->heads[index]; e != UINT32_MAX; e = origins->edges[e].next) {
                EffectCheckOriginEdge edge = origins->edges[e];
                if (edge.result && (query == EXECUTION_ORIGINS_EFFECTS || query == EXECUTION_ORIGINS_CALLABLE)) continue;
                if (edge.bottom) {
                    found |= walk.observe_environment;
                    predecessor = true;
                    continue;
                }
                if (edge.parameter_contents) {
                    bool input = execution_origin_input(&walk,roots,&storage,origin,edge,&unresolved);
                    if (walk.status != XR_XIR_OK) break;
                    found |= input;
                    predecessor = true;
                    continue;
                }
                if (edge.application != UINT32_MAX) {
                    /* A target visitor can append guard applications within
                     * this cursor. Rebind before reading the receiving bank. */
                    walk.status = xir_storage_flow_view(roots->storage, &storage);
                    if (walk.status != XR_XIR_OK) break;
                    if (edge.application >= storage.application_count) {walk.status = XR_XIR_BAD_STRUCTURE; break;}
                    XirStorageApplication app = storage.applications[edge.application];
                    if (edge.result && app.caller_instance != origin.function) continue;
                    if (app.function == function.declaration && app.environment != origin.environment) {
                        if (app.environment == XIR_EXECUTION_CURRENT_ENVIRONMENT)
                            unresolved |= edge.result && origin.storable ?
                                XIR_EXECUTION_REMAINDER_STORABLE_ENVIRONMENT : XIR_EXECUTION_REMAINDER_STORAGE;
                        continue;
                    }
                }
                if (edge.function == UINT32_MAX) {
                    if (walk.footprint && roots->storage && !origin.equation) {
                        bool closed = false;
                        walk.status = xir_storage_flow_environment_closed(roots->storage,
                            function.declaration, origin.value, origin.function, &closed);
                        if (walk.status != XR_XIR_OK) break;
                        if (closed) {found = true; predecessor = true; continue;}
                    }
                    unresolved |= origin.storable ? XIR_EXECUTION_REMAINDER_STORABLE_ENVIRONMENT :
                        XIR_EXECUTION_REMAINDER_OPAQUE;
                    predecessor = true;
                    continue;
                }
                uint32_t next_environment = edge.application == UINT32_MAX ? origin.environment : edge.application;
                uint32_t source_function = !edge.result && edge.function == function.declaration ? origin.function : edge.function;
                execution_origin_push(&walk, (ExecutionOrigin){source_function, edge.value,
                    next_environment, edge.result, origin.entry_context, origin.storable});
                predecessor = true;
            }
        }
        if (!predecessor) unresolved |= origin.storable ? XIR_EXECUTION_REMAINDER_STORABLE_ENVIRONMENT :
            XIR_EXECUTION_REMAINDER_STORAGE;
    }
    if (!found && !unresolved) unresolved = XIR_EXECUTION_REMAINDER_STORAGE;
    if (walk.status == XR_XIR_OK) {*complete = found && !unresolved; *remainder = unresolved;}
    xr_compile_resources_free(walk.items);
    xr_compile_resources_free(walk.index);
    return walk.status;
}

XR_FUNC uint64_t xir_execution_storage_revision(const XirExecutionOwner *owner) {
    /* Contents inputs precede the first receipt. Later origin edges append;
     * binding replacements invalidate storage separately. Empty aux values
     * add no source dependency and must not replay every application stage. */
    return owner ? owner->storage_origins.count : 0;
}

XR_FUNC XrXirStatus xir_execution_refresh_storage(XirExecutionOwner *owner) {
    if (!owner || !owner->storage) return XR_XIR_BAD_STRUCTURE;
    XirStorageFlowView storage = {0};
    XrXirStatus status = xir_storage_flow_view(owner->storage, &storage);
    XrXirCompileContext memory = {.resources = owner->resources};
    return status == XR_XIR_OK ? execution_origins_receive(&memory, owner, &storage) : status;
}

static XrXirStatus execution_visit_value_environment(const XirExecutionOwner *owner,
    const XirExecutionValue *value, XirExecutionVisitor visit, void *context, bool *complete,
    uint32_t *remainder, ExecutionOriginQuery query) {
    if (!value || !owner || value->function >= owner->templates) return XR_XIR_BAD_STRUCTURE;
    uint32_t count = owner->function[value->function].values;
    if (owner->storage) {
        XirStorageFlowView storage = {0};
        XrXirStatus status = xir_storage_flow_view(owner->storage, &storage);
        if (status != XR_XIR_OK) return status;
        if (value->function >= storage.function_count) return XR_XIR_BAD_STRUCTURE;
        count = storage.functions[value->function].value_count;
    }
    if (value->value >= count) return XR_XIR_BAD_STRUCTURE;
    uint32_t pending = 0;
    return execution_visit_origins(owner, value->function, value->value, value->environment,
        visit, context, complete, remainder ? remainder : &pending, (ExecutionOriginRequest){.kind = query});
}

XR_FUNC XrXirStatus xir_execution_visit_value_captures_in(const XirExecutionOwner *owner,
    const XirExecutionValue *value, XirExecutionVisitor visit, void *context, bool *complete, uint32_t *remainder) {
    return execution_visit_value_environment(owner, value, visit, context, complete, remainder, EXECUTION_ORIGINS_CALLABLE);
}

XR_FUNC XrXirStatus xir_execution_visit_value_environment_in(const XirExecutionOwner *owner,
    const XirExecutionValue *value, XirExecutionVisitor visit, void *context, bool *complete, uint32_t *remainder) {
    return execution_visit_value_environment(owner, value, visit, context, complete, remainder, EXECUTION_ORIGINS_ENVIRONMENT);
}

XR_FUNC XrXirStatus xir_execution_visit_value_environment_inputs_in(const XirExecutionOwner *owner,
    const XirExecutionValue *value, XirExecutionVisitor visit, void *context, bool *complete, uint32_t *remainder) {
    return execution_visit_value_environment(owner, value, visit, context, complete, remainder,
        EXECUTION_ORIGINS_ENVIRONMENT_INPUTS);
}

XR_FUNC XrXirStatus xir_execution_visit_value_footprint_in(const XirExecutionOwner *owner,
    const XirExecutionValue *value, XirExecutionVisitor visit, void *context, bool *complete, uint32_t *remainder) {
    return execution_visit_value_environment(owner,value,visit,context,complete,remainder,EXECUTION_ORIGINS_FOOTPRINT);
}

typedef struct ExecutionResolveContext {
    const EffectCheckedRoots *roots;
    const XirExecutionSite *site;
    uint32_t environment;
    XirExecutionVisitor visit;
    void *context;
} ExecutionResolveContext;

static XrXirStatus execution_resolve_capture(void *context, const XirExecutionSite *capture) {
    ExecutionResolveContext *resolve = context;
    if (capture->argument_count > UINT32_MAX - resolve->site->argument_count) return XR_XIR_OVERFLOW;
    XirExecutionSite site = execution_resolved_site(resolve->roots, resolve->site,
        capture->index, capture->environment);
    site.environment = resolve->environment;
    return resolve->visit(resolve->context, &site);
}

XR_FUNC XrXirStatus xir_execution_resolve_site(const XirExecutionOwner *owner,
    const XirExecutionSite *site, uint32_t environment, XirExecutionVisitor visit, void *context,
    bool *complete, uint32_t *remainder) {
    if (!owner || !site || !visit || !complete || !remainder ||
        site->kind != XIR_EXECUTION_PENDING || site->index >= owner->pending_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectCheckPending pending = owner->pending[site->index];
    if (pending.kind != EFFECT_CHECK_HIGHER_ORDER) {
        *complete = false; *remainder = XIR_EXECUTION_REMAINDER_OPAQUE; return XR_XIR_OK;
    }
    XirExecutionSite request = *site;
    const EffectCheckFunction *receiver = &owner->function[owner->node[pending.receiver].function];
    ExecutionResolveContext resolve = {owner, &request, environment, visit, context};
    return execution_visit_origins(owner, owner->node[pending.receiver].function,
        pending.receiver - receiver->first, environment, execution_resolve_capture, &resolve,
        complete, remainder, (ExecutionOriginRequest){.kind = EXECUTION_ORIGINS_CALLABLE});
}

XR_FUNC XrXirStatus xir_execution_resolve_effect_site(const XirExecutionOwner *owner,
    const XirExecutionSite *site, XirExecutionVisitor visit, void *context,
    bool *complete, uint32_t *remainder) {
    if (!owner || !site || !visit || !complete || !remainder ||
        site->kind != XIR_EXECUTION_PENDING || site->index >= owner->pending_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectCheckPending pending = owner->pending[site->index];
    if (pending.kind != EFFECT_CHECK_HIGHER_ORDER) return XR_XIR_BAD_STRUCTURE;
    XirExecutionSite request = *site;
    const EffectCheckFunction *receiver = &owner->function[owner->node[pending.receiver].function];
    ExecutionResolveContext resolve = {owner, &request, XIR_EXECUTION_CURRENT_ENVIRONMENT, visit, context};
    return execution_visit_origins(owner, owner->node[pending.receiver].function,
        pending.receiver - receiver->first, XIR_EXECUTION_CURRENT_ENVIRONMENT,
        execution_resolve_capture, &resolve, complete, remainder, (ExecutionOriginRequest){.kind = EXECUTION_ORIGINS_EFFECTS});
}
