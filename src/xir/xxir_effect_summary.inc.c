/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_summary.inc.c - Owned control dependencies and SCC closure
 *
 * KEY CONCEPT:
 *   Body instructions contribute local facts once. Closure reads only numeric
 *   dependencies, including deferred virtual slots, and owns no body snapshot.
 */
enum {
    EFFECT_SUMMARY_UNKNOWN = 1,
    EFFECT_SUMMARY_SUSPEND = 2,
    EFFECT_SUMMARY_SPAWN = 4
};

typedef enum EffectSummaryNodeKind {
    EFFECT_SUMMARY_FUNCTION, EFFECT_SUMMARY_VALUE, EFFECT_SUMMARY_DISPATCH
} EffectSummaryNodeKind;

typedef struct EffectSummaryNode {
    uint32_t head;
    uint32_t terminals[2];
    uint32_t function;
    uint8_t local, closed, kind, throws, collected;
} EffectSummaryNode;

typedef struct EffectSummaryEdge {
    uint32_t caller, target, next, instruction;
} EffectSummaryEdge;

typedef struct EffectSummaryDispatch {
    XrXirType receiver;
    uint32_t slot, node;
} EffectSummaryDispatch;

typedef struct EffectControlGraph {
    uint32_t node_count, node_capacity, edge_count, edge_capacity;
    uint32_t dispatch_count, dispatch_capacity;
    EffectSummaryNode *nodes;
    EffectSummaryEdge *edges;
    EffectSummaryDispatch *dispatches;
} EffectControlGraph;

typedef struct EffectSummaryDispatchSource {
    void *owner;
    XrXirStatus (*read)(void *, XrXirType, uint32_t, XirClassMethodVisitor, void *);
} EffectSummaryDispatchSource;

struct EffectControlSummary {
    atomic_uint_fast64_t references;
    XrCompileResources *resources;
    uint32_t functions;
    XrXirFunctionEffects *facts;
    XrXirEffectWitness *causes;
    XrXirEffect *task_creation;
};

typedef struct EffectSummaryBuild {
    XrXirCompileContext context;
    const XrXirCompileContext *memory;
    const XrXirModule *input;
    XrCompileRequestControl *control;
    EffectControlSummary *summary;
    EffectControlGraph graph;
    EffectSummaryDispatchSource dispatch_source;
    uint32_t *dispatch_index;
    size_t *default_offsets;
    uint32_t *default_targets;
    uint32_t dispatch_index_capacity, progress;
    uint32_t *function_nodes, function_capacity;
    uint32_t *values, active_function, active_parameters, active_instructions, next_instruction;
    bool active, finished;
    XrXirStatus status;
} EffectSummaryBuild;

XR_FUNC void xir_effect_control_free(EffectControlSummary *summary) {
    if (!summary) return;
    uint_fast64_t previous = atomic_fetch_sub_explicit(&summary->references, 1, memory_order_acq_rel);
    XR_CHECK(previous, "A control summary must be released exactly once per owner");
    if (previous != 1) return;
    xr_compile_resources_free(summary->facts);
    xr_compile_resources_free(summary->causes);
    xr_compile_resources_free(summary->task_creation);
    xr_compile_resources_free(summary);
}

XR_FUNC XrXirStatus xir_effect_control_retain(const XrXirCompileContext *context,
    EffectControlSummary *summary, EffectControlSummary **output) {
    if (!xir_compile_context_valid(context) || !summary || !output || *output ||
        summary->resources != context->resources) return XR_XIR_BAD_STRUCTURE;
    uint_fast64_t references = atomic_load_explicit(&summary->references, memory_order_relaxed);
    for (;;) {
        if (!references || references == UINT_FAST64_MAX) return XR_XIR_OVERFLOW;
        if (atomic_compare_exchange_weak_explicit(&summary->references, &references, references + 1,
            memory_order_relaxed, memory_order_relaxed)) break;
    }
    *output = summary;
    return XR_XIR_OK;
}

static bool effect_summary_poll(EffectSummaryBuild *build) {
    if (build->status == XR_XIR_OK)
        build->status = xir_receive_step(build->control, &build->progress);
    return build->status == XR_XIR_OK;
}

static void *effect_summary_grow(EffectSummaryBuild *build, void *old,
    uint32_t count, size_t element, uint32_t *capacity) {
    return xir_compile_array_grow(build->memory, old, count, element, capacity, &build->status);
}

static uint32_t effect_summary_node(EffectSummaryBuild *build, EffectSummaryNodeKind kind) {
    EffectControlGraph *graph = &build->graph;
    if (graph->node_count == graph->node_capacity) {
        EffectSummaryNode *nodes = effect_summary_grow(build, graph->nodes,
            graph->node_count, sizeof(*nodes), &graph->node_capacity);
        if (!nodes) return UINT32_MAX;
        graph->nodes = nodes;
    }
    uint32_t index = graph->node_count++;
    graph->nodes[index] = (EffectSummaryNode){.head = UINT32_MAX, .terminals = {UINT32_MAX, UINT32_MAX},
        .kind = (uint8_t)kind};
    return index;
}

static bool effect_summary_edge(EffectSummaryBuild *build, uint32_t caller,
    uint32_t target, uint32_t instruction) {
    EffectControlGraph *graph = &build->graph;
    if (caller >= graph->node_count || target >= graph->node_count) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    if (graph->edge_count == graph->edge_capacity) {
        EffectSummaryEdge *edges = effect_summary_grow(build, graph->edges,
            graph->edge_count, sizeof(*edges), &graph->edge_capacity);
        if (!edges) return false;
        graph->edges = edges;
    }
    uint32_t index = graph->edge_count++;
    graph->edges[index] = (EffectSummaryEdge){caller, target,
        graph->nodes[caller].head, instruction};
    graph->nodes[caller].head = index;
    return true;
}

static uint32_t effect_summary_function_node(EffectSummaryBuild *build, uint32_t function) {
    while (function >= build->function_capacity) {
        uint32_t previous = build->function_capacity;
        uint32_t *nodes = effect_summary_grow(build, build->function_nodes,
            previous, sizeof(*nodes), &build->function_capacity);
        if (!nodes) return UINT32_MAX;
        build->function_nodes = nodes;
        for (uint32_t f = previous; f < build->function_capacity; ++f) nodes[f] = UINT32_MAX;
    }
    if (build->function_nodes[function] == UINT32_MAX) {
        uint32_t node = effect_summary_node(build, EFFECT_SUMMARY_FUNCTION);
        if (node == UINT32_MAX) return node;
        build->function_nodes[function] = node;
        build->graph.nodes[node].function = function;
    }
    return build->function_nodes[function];
}

static void effect_summary_seed(EffectControlGraph *graph, uint32_t node,
    uint8_t mask, uint32_t instruction) {
    EffectSummaryNode *row = &graph->nodes[node];
    if ((mask & EFFECT_SUMMARY_UNKNOWN) && !(row->local & EFFECT_SUMMARY_UNKNOWN))
        row->terminals[0] = instruction;
    if ((mask & EFFECT_SUMMARY_SUSPEND) && !(row->local & EFFECT_SUMMARY_SUSPEND))
        row->terminals[1] = instruction;
    row->local |= mask;
}

static uint32_t effect_summary_dispatch_hash(XrXirType receiver, uint32_t slot) {
    uint32_t hash = (uint32_t)receiver * UINT32_C(0x9e3779b9);
    hash ^= slot + UINT32_C(0x85ebca6b) + (hash << 6) + (hash >> 2);
    return hash;
}

static bool effect_summary_dispatch_index(EffectSummaryBuild *build) {
    EffectControlGraph *graph = &build->graph;
    if (build->dispatch_index_capacity &&
        graph->dispatch_count < build->dispatch_index_capacity / 2) return true;
    uint64_t capacity = build->dispatch_index_capacity ?
        (uint64_t)build->dispatch_index_capacity * 2 : 64;
    if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof(uint32_t)) {
        build->status = XR_XIR_OVERFLOW;
        return false;
    }
    uint32_t *index = xir_compile_calloc(build->memory, (size_t)capacity,
        sizeof(*index), &build->status);
    if (!index) return false;
    for (uint32_t d = 0; d < graph->dispatch_count; ++d) {
        const EffectSummaryDispatch *entry = &graph->dispatches[d];
        uint32_t at = effect_summary_dispatch_hash(entry->receiver, entry->slot) & ((uint32_t)capacity - 1);
        while (index[at]) at = (at + 1) & ((uint32_t)capacity - 1);
        index[at] = d + 1;
    }
    xr_compile_resources_free(build->dispatch_index);
    build->dispatch_index = index;
    build->dispatch_index_capacity = (uint32_t)capacity;
    return true;
}

static uint32_t effect_summary_dispatch(EffectSummaryBuild *build, XrXirType receiver, uint32_t slot) {
    if (!effect_summary_dispatch_index(build)) return UINT32_MAX;
    EffectControlGraph *graph = &build->graph;
    uint32_t mask = build->dispatch_index_capacity - 1;
    uint32_t at = effect_summary_dispatch_hash(receiver, slot) & mask;
    while (build->dispatch_index[at]) {
        const EffectSummaryDispatch *entry = &graph->dispatches[build->dispatch_index[at] - 1];
        if (entry->receiver == receiver && entry->slot == slot) return entry->node;
        at = (at + 1) & mask;
    }
    if (graph->dispatch_count == graph->dispatch_capacity) {
        EffectSummaryDispatch *entries = effect_summary_grow(build, graph->dispatches,
            graph->dispatch_count, sizeof(*entries), &graph->dispatch_capacity);
        if (!entries) return UINT32_MAX;
        graph->dispatches = entries;
    }
    uint32_t node = effect_summary_node(build, EFFECT_SUMMARY_DISPATCH);
    if (node == UINT32_MAX) return node;
    graph->dispatches[graph->dispatch_count] = (EffectSummaryDispatch){receiver, slot, node};
    build->dispatch_index[at] = ++graph->dispatch_count;
    return node;
}

static uint32_t effect_summary_value(EffectSummaryBuild *build, uint32_t function,
    uint32_t value, uint32_t *values) {
    const XrXirFunction *body = &build->input->functions[function];
    if (value >= (uint64_t)body->parameter_count + body->instruction_count) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return UINT32_MAX;
    }
    if (values[value] == UINT32_MAX) {
        values[value] = effect_summary_node(build, EFFECT_SUMMARY_VALUE);
        if (values[value] != UINT32_MAX &&
            !xr_xir_type_is_callable(build->input->types, xr_xir_operand_type(body, value)))
            effect_summary_seed(&build->graph, values[value], EFFECT_SUMMARY_UNKNOWN, UINT32_MAX);
    }
    return values[value];
}

static bool effect_summary_value_edge(EffectSummaryBuild *build, uint32_t function,
    uint32_t caller, uint32_t value, uint32_t *values, uint32_t instruction) {
    uint32_t target = effect_summary_value(build, function, value, values);
    return target != UINT32_MAX && effect_summary_edge(build, caller, target, instruction);
}

static bool effect_summary_value_flow(EffectSummaryBuild *build, uint32_t function,
    uint32_t instruction, uint32_t *values) {
    const XrXirFunction *body = &build->input->functions[function];
    const XrXirInstruction *op = &body->instructions[instruction];
    uint32_t node = effect_summary_value(build, function, body->parameter_count + instruction, values);
    if (node == UINT32_MAX) return false;
    const XrXirTypeNode *signature = xr_xir_callable_signature(build->input->types, op->type);
    if (!signature) { build->status = XR_XIR_BAD_TYPE; return false; }
    if (signature->flags & XR_XIR_CALLABLE_NO_SUSPEND) return true;
    switch (op->op) {
    case XR_XIR_CLASS_METHOD_REF: {
        XrXirType receiver = xr_xir_operand_type(body, op->args[0]);
        uint32_t dispatch = effect_summary_dispatch(build, receiver, (uint32_t)op->immediate);
        return dispatch != UINT32_MAX && effect_summary_edge(build, node, dispatch, instruction);
    }
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ:
    case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_READ:
    case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ:
        return effect_summary_value_edge(build, function, node, op->args[0], values, instruction);
    case XR_XIR_LOCAL_UNINIT:
        effect_summary_seed(&build->graph, node, EFFECT_SUMMARY_UNKNOWN, instruction);
        return true;
    case XR_XIR_PHI:
        if (op->args[1] % 2 || op->args[0] > body->operand_count ||
            op->args[1] > body->operand_count - op->args[0]) {
            build->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        for (uint32_t p = 1; p < op->args[1]; p += 2)
            if (!effect_summary_value_edge(build, function, node,
                    body->operands[op->args[0] + p], values, instruction)) return false;
        return true;
    default:
        effect_summary_seed(&build->graph, node, EFFECT_SUMMARY_UNKNOWN, instruction);
        return true;
    }
}

static bool effect_summary_defaults(EffectSummaryBuild *build) {
    const XrXirModule *module = build->input;
    if (!module->defaults || !module->defaults->count) return true;
    if ((uint64_t)module->function_count + 1 > SIZE_MAX / sizeof(size_t)) {
        build->status = XR_XIR_OVERFLOW;
        return false;
    }
    build->default_offsets = xir_compile_calloc(build->memory,
        (size_t)module->function_count + 1, sizeof(*build->default_offsets), &build->status);
    if (!build->default_offsets) return false;
    size_t count = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (module->functions[f].parameter_count > SIZE_MAX - count) {
            build->status = XR_XIR_OVERFLOW;
            return false;
        }
        count += module->functions[f].parameter_count;
        build->default_offsets[f + 1] = count;
    }
    if (!count || count > SIZE_MAX / sizeof(*build->default_targets)) {
        build->status = count ? XR_XIR_OVERFLOW : XR_XIR_BAD_STRUCTURE;
        return false;
    }
    build->default_targets = xir_compile_alloc(build->memory,
        count * sizeof(*build->default_targets), &build->status);
    if (!build->default_targets) return false;
    for (size_t p = 0; p < count; ++p) build->default_targets[p] = UINT32_MAX;
    for (uint32_t d = 0; d < module->defaults->count; ++d) {
        const XrXirDefaultBinding *binding = &module->defaults->records[d];
        if (binding->owner_kind != XR_XIR_DEFAULT_PARAMETER || binding->owner >= module->function_count ||
            binding->ordinal >= module->functions[binding->owner].parameter_count ||
            binding->function >= module->function_count) {
            build->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        size_t at = build->default_offsets[binding->owner] + binding->ordinal;
        if (build->default_targets[at] != UINT32_MAX) {
            build->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        build->default_targets[at] = binding->function;
    }
    return true;
}

static bool effect_summary_instruction(EffectSummaryBuild *build, uint32_t function,
    uint32_t instruction, uint32_t *values) {
    const XrXirFunction *body = &build->input->functions[function];
    const XrXirInstruction *op = &body->instructions[instruction];
    if (!effect_summary_poll(build)) return false;
    uint32_t caller = effect_summary_function_node(build, function);
    if (caller == UINT32_MAX) return false;
    XrXirFunctionEffects seed = {0};
    EffectSeedProjection projection = effect_seed_opcode(op->op, &seed);
    if (projection == EFFECT_SEED_INVALID) { build->status = XR_XIR_BAD_STRUCTURE; return false; }
    if (seed.suspend == XR_XIR_EFFECT_MAY)
        effect_summary_seed(&build->graph, caller, EFFECT_SUMMARY_SUSPEND, instruction);
    if (xr_xir_op_is_spawn(op->op))
        effect_summary_seed(&build->graph, caller, EFFECT_SUMMARY_SPAWN, instruction);
    if (xr_xir_type_is_callable(build->input->types, op->type) &&
        !effect_summary_value_flow(build, function, instruction, values)) return false;
    if (op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE) {
        if (op->immediate < 0 || (uint64_t)op->immediate >= build->input->function_count) {
            build->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        uint32_t target = effect_summary_function_node(build, (uint32_t)op->immediate);
        return target != UINT32_MAX && effect_summary_edge(build, caller, target, instruction);
    }
    if (op->op == XR_XIR_CALL_DEFAULT || op->op == XR_XIR_INVOKE_DEFAULT) {
        const uint32_t *identity = xr_xir_default_identity(op);
        if (!build->default_offsets || identity[0] >= build->input->function_count ||
            identity[1] >= build->input->functions[identity[0]].parameter_count) {
            build->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        uint32_t target = build->default_targets[build->default_offsets[identity[0]] + identity[1]];
        if (target == UINT32_MAX) { build->status = XR_XIR_BAD_STRUCTURE; return false; }
        target = effect_summary_function_node(build, target);
        return target != UINT32_MAX && effect_summary_edge(build, caller, target, instruction);
    }
    if (op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT)
        return effect_summary_value_edge(build, function, caller,
            (uint32_t)op->immediate, values, instruction);
    if (op->op == XR_XIR_CALL_REQUIREMENT) {
        if (!effect_seed_type(build->input->types, XR_XIR_UNIT, op, &seed)) {
            build->status = XR_XIR_BAD_TYPE;
            return false;
        }
        if (seed.suspend == XR_XIR_EFFECT_UNKNOWN)
            effect_summary_seed(&build->graph, caller, EFFECT_SUMMARY_UNKNOWN, instruction);
    }
    if (op->op == XR_XIR_LOCAL_WRITE || op->op == XR_XIR_SCALAR_LOCAL_WRITE ||
        op->op == XR_XIR_OWNED_LOCAL_WRITE) {
        if (xr_xir_type_is_callable(build->input->types, xr_xir_operand_type(body, op->args[0]))) {
            uint32_t local = effect_summary_value(build, function, op->args[0], values);
            return local != UINT32_MAX && effect_summary_value_edge(build, function,
                local, op->args[1], values, instruction);
        }
    }
    return true;
}

XR_FUNC XrXirStatus xir_effect_control_begin(XirControlBuilder *build,
    const XrXirModule *module, uint32_t function) {
    if (!build || !module || !module->functions || function >= module->function_count ||
        build->active || build->finished) return XR_XIR_BAD_STRUCTURE;
    if (build->status != XR_XIR_OK) return build->status;
    build->input = module;
    uint32_t caller = effect_summary_function_node(build, function);
    if (caller == UINT32_MAX) { build->input = NULL; return build->status; }
    if (build->graph.nodes[caller].collected) { build->input = NULL; return XR_XIR_BAD_STRUCTURE; }
    const XrXirFunction *body = &module->functions[function];
    uint64_t count = (uint64_t)body->parameter_count + body->instruction_count;
    if (count > UINT32_MAX || count > SIZE_MAX / sizeof(uint32_t)) {
        build->status = XR_XIR_OVERFLOW;
        build->input = NULL;
        return build->status;
    }
    uint32_t *values = count ? xir_compile_alloc(build->memory,
        (size_t)count * sizeof(*values), &build->status) : NULL;
    if (count && !values) { build->input = NULL; return build->status; }
    build->values = values;
    build->active = true;
    build->active_function = function;
    build->active_parameters = body->parameter_count;
    build->active_instructions = body->instruction_count;
    build->next_instruction = 0;
    for (uint32_t i = 0; i < (uint32_t)count; ++i) values[i] = UINT32_MAX;
    for (uint32_t p = 0; p < body->parameter_count && build->status == XR_XIR_OK; ++p) {
        const XrXirTypeNode *signature = xr_xir_callable_signature(module->types, body->parameters[p]);
        if (!signature) continue;
        uint32_t node = effect_summary_value(build, function, p, values);
        if (node == UINT32_MAX) break;
        if (!(signature->flags & XR_XIR_CALLABLE_NO_SUSPEND))
            effect_summary_seed(&build->graph, node, EFFECT_SUMMARY_UNKNOWN, UINT32_MAX);
    }
    if (module->declarations && module->declarations->functions &&
        xir_sys_pipe_can_suspend(module->declarations->functions[function].native_callable))
        effect_summary_seed(&build->graph, caller, EFFECT_SUMMARY_SUSPEND, UINT32_MAX);
    build->input = NULL;
    return build->status;
}

static bool effect_summary_active(const EffectSummaryBuild *build,
    const XrXirModule *module, uint32_t function) {
    return build && build->active && !build->finished && module && module->functions &&
        function < module->function_count && function == build->active_function &&
        module->functions[function].parameter_count == build->active_parameters &&
        module->functions[function].instruction_count == build->active_instructions;
}

XR_FUNC XrXirStatus xir_effect_control_instruction(XirControlBuilder *build,
    const XrXirModule *module, uint32_t function, uint32_t instruction) {
    if (!effect_summary_active(build, module, function) || instruction != build->next_instruction ||
        instruction >= build->active_instructions) return XR_XIR_BAD_STRUCTURE;
    if (build->status != XR_XIR_OK) return build->status;
    build->input = module;
    if (!effect_summary_instruction(build, function, instruction, build->values) && build->status == XR_XIR_OK)
        build->status = XR_XIR_BAD_STRUCTURE;
    build->input = NULL;
    if (build->status == XR_XIR_OK) ++build->next_instruction;
    return build->status;
}

XR_FUNC XrXirStatus xir_effect_control_end(XirControlBuilder *build,
    const XrXirModule *module, uint32_t function) {
    if (!effect_summary_active(build, module, function) ||
        build->next_instruction != build->active_instructions) return XR_XIR_BAD_STRUCTURE;
    if (build->status != XR_XIR_OK) return build->status;
    const XrXirFunction *body = &module->functions[function];
    XrXirType error = body->error_type;
    if (module->declarations && module->declarations->functions &&
        (module->declarations->functions[function].promises & XR_XIR_FUNCTION_GENERATOR)) {
        const XrXirTypeNode *iterator = xr_xir_type_node(module->types, body->result);
        if (!iterator || iterator->kind != XR_XIR_TYPE_ITERATOR) build->status = XR_XIR_BAD_TYPE;
        else error = iterator->result;
    }
    if (error == XR_XIR_UNIT) build->status = XR_XIR_BAD_TYPE;
    EffectSummaryNode *node = &build->graph.nodes[build->function_nodes[function]];
    node->throws = error == XR_XIR_NEVER ? XR_XIR_EFFECT_NONE : XR_XIR_EFFECT_MAY;
    node->collected = 1;
    xr_compile_resources_free(build->values);
    build->values = NULL;
    build->active = false;
    return build->status;
}

typedef struct EffectSummaryCandidate {
    EffectSummaryBuild *build;
    uint32_t dispatch;
} EffectSummaryCandidate;

static XrXirStatus effect_summary_candidate(void *owner, const XirClassMethodTarget *target) {
    EffectSummaryCandidate *candidate = owner;
    uint32_t node = effect_summary_function_node(candidate->build, target->function);
    return node != UINT32_MAX && effect_summary_edge(candidate->build, candidate->dispatch, node,
        UINT32_MAX) ? XR_XIR_OK : candidate->build->status;
}

static bool effect_summary_link(EffectSummaryBuild *build) {
    EffectControlGraph *graph = &build->graph;
    for (uint32_t d = 0; d < graph->dispatch_count; ++d) {
        if (!effect_summary_poll(build)) return false;
        EffectSummaryDispatch dispatch = graph->dispatches[d];
        if (build->input->linkage_kind != XR_XIR_PROGRAM) {
            effect_summary_seed(graph, dispatch.node, EFFECT_SUMMARY_UNKNOWN, UINT32_MAX);
            continue;
        }
        EffectSummaryCandidate candidate = {build, dispatch.node};
        build->status = build->dispatch_source.read ?
            build->dispatch_source.read(build->dispatch_source.owner, dispatch.receiver,
                dispatch.slot, effect_summary_candidate, &candidate) :
            xir_compile_class_method_candidates(build->input, dispatch.receiver,
                dispatch.slot, effect_summary_candidate, &candidate, build->control);
        if (build->status != XR_XIR_OK) return false;
    }
    return true;
}

static bool effect_summary_close(EffectSummaryBuild *build) {
    EffectControlGraph *graph = &build->graph;
    uint64_t node_bytes = (uint64_t)graph->node_count * sizeof(XirEffectSccNode);
    uint64_t edge_bytes = (uint64_t)graph->edge_count * sizeof(XirEffectSccEdge);
    if (node_bytes > SIZE_MAX || edge_bytes > SIZE_MAX) {
        build->status = XR_XIR_OVERFLOW;
        return false;
    }
    XirEffectSccNode *nodes = xir_compile_alloc(build->memory,
        (size_t)node_bytes, &build->status);
    XirEffectSccEdge *edges = graph->edge_count ? xir_compile_alloc(build->memory,
        (size_t)edge_bytes, &build->status) : NULL;
    if (nodes && (!graph->edge_count || edges)) {
        for (uint32_t n = 0; n < graph->node_count; ++n)
            nodes[n] = (XirEffectSccNode){graph->nodes[n].head, graph->nodes[n].local, 0};
        for (uint32_t e = 0; e < graph->edge_count; ++e)
            edges[e] = (XirEffectSccEdge){graph->edges[e].target, graph->edges[e].next};
        build->status = xir_effect_scc_close(build->memory, nodes, graph->node_count,
            edges, graph->edge_count, build->control);
        for (uint32_t n = 0; n < graph->node_count && build->status == XR_XIR_OK; ++n)
            graph->nodes[n].closed = (uint8_t)nodes[n].closed;
    }
    xr_compile_resources_free(nodes);
    xr_compile_resources_free(edges);
    return build->status == XR_XIR_OK;
}

static uint8_t effect_summary_suspension(uint8_t mask) {
    return mask & EFFECT_SUMMARY_SUSPEND ? EFFECT_SUMMARY_SUSPEND : mask & EFFECT_SUMMARY_UNKNOWN;
}

typedef struct EffectSummaryReason {
    uint32_t function, distance;
    bool ready;
} EffectSummaryReason;

static bool effect_summary_causes(EffectSummaryBuild *build) {
    EffectControlGraph *graph = &build->graph;
    EffectControlSummary *result = build->summary;
    uint32_t count = graph->node_count;
    uint32_t *heads = xir_compile_alloc(build->memory, (size_t)count * sizeof(*heads), &build->status);
    uint32_t *queue = xir_compile_alloc(build->memory, (size_t)count * sizeof(*queue), &build->status);
    uint32_t *next = graph->edge_count ? xir_compile_alloc(build->memory,
        (size_t)graph->edge_count * sizeof(*next), &build->status) : NULL;
    EffectSummaryReason *reasons = xir_compile_calloc(build->memory, count, sizeof(*reasons), &build->status);
    if (!heads || !queue || (graph->edge_count && !next) || !reasons) goto done;
    for (uint32_t n = 0; n < count; ++n) heads[n] = UINT32_MAX;
    for (uint32_t e = graph->edge_count; e-- > 0;) {
        uint32_t target = graph->edges[e].target;
        next[e] = heads[target];
        heads[target] = e;
    }
    uint32_t front = 0, back = 0;
    for (uint32_t n = 0; n < count; ++n) {
        const EffectSummaryNode *node = &graph->nodes[n];
        uint8_t suspension = effect_summary_suspension(node->closed);
        if (!suspension || !(node->local & suspension)) continue;
        bool function = node->kind == EFFECT_SUMMARY_FUNCTION;
        reasons[n] = (EffectSummaryReason){function ? node->function : UINT32_MAX, 0, true};
        if (function) result->causes[node->function] = (XrXirEffectWitness){
            suspension == EFFECT_SUMMARY_SUSPEND ? XR_XIR_EFFECT_CAUSE_SUSPEND : XR_XIR_EFFECT_CAUSE_INDIRECT,
            node->terminals[suspension == EFFECT_SUMMARY_SUSPEND], UINT32_MAX, 0};
        queue[back++] = n;
    }
    while (front < back && effect_summary_poll(build)) {
        uint32_t target = queue[front++];
        for (uint32_t e = heads[target]; e != UINT32_MAX; e = next[e]) {
            const EffectSummaryEdge *edge = &graph->edges[e];
            uint32_t caller = edge->caller;
            if (reasons[caller].ready || effect_summary_suspension(graph->nodes[caller].closed) !=
                effect_summary_suspension(graph->nodes[target].closed)) continue;
            EffectSummaryReason reason = reasons[target];
            if (graph->nodes[caller].kind == EFFECT_SUMMARY_FUNCTION) {
                uint32_t distance = reason.function == UINT32_MAX ? 0 : reason.distance + 1;
                uint32_t function = graph->nodes[caller].function;
                result->causes[function] = (XrXirEffectWitness){
                    reason.function == UINT32_MAX ? XR_XIR_EFFECT_CAUSE_INDIRECT : XR_XIR_EFFECT_CAUSE_CALL,
                    edge->instruction, reason.function, distance};
                reason.function = function;
                reason.distance = distance;
            }
            reasons[caller] = reason;
            queue[back++] = caller;
        }
    }
    for (uint32_t f = 0; f < result->functions && build->status == XR_XIR_OK; ++f) {
        uint32_t node = build->function_nodes[f];
        uint8_t suspension = effect_summary_suspension(graph->nodes[node].closed);
        if (suspension && !reasons[node].ready) build->status = XR_XIR_BAD_STRUCTURE;
        result->facts[f].suspend = suspension == EFFECT_SUMMARY_SUSPEND ? XR_XIR_EFFECT_MAY :
            suspension ? XR_XIR_EFFECT_UNKNOWN : XR_XIR_EFFECT_NONE;
        result->facts[f].throws = (XrXirEffect)graph->nodes[node].throws;
        result->task_creation[f] = graph->nodes[node].closed & EFFECT_SUMMARY_SPAWN ?
            XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_NONE;
    }
done:
    xr_compile_resources_free(heads);
    xr_compile_resources_free(queue);
    xr_compile_resources_free(next);
    xr_compile_resources_free(reasons);
    return build->status == XR_XIR_OK;
}

XR_FUNC void xir_effect_control_builder_free(XirControlBuilder *build) {
    if (!build) return;
    xir_effect_control_free(build->summary);
    xr_compile_resources_free(build->values);
    xr_compile_resources_free(build->function_nodes);
    xr_compile_resources_free(build->dispatch_index);
    xr_compile_resources_free(build->default_offsets);
    xr_compile_resources_free(build->default_targets);
    xr_compile_resources_free(build->graph.nodes);
    xr_compile_resources_free(build->graph.edges);
    xr_compile_resources_free(build->graph.dispatches);
    xr_compile_resources_free(build);
}

XR_FUNC XrXirStatus xir_effect_control_builder_new(const XrXirCompileContext *memory,
    const XrXirModule *module, XrCompileRequestControl *control, XirControlBuilder **output) {
    if (!xir_compile_context_valid(memory) || !module || !module->function_count ||
        !module->functions || !output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    EffectSummaryBuild *build = xir_compile_calloc(memory, 1, sizeof(*build), &status);
    if (!build) return status;
    build->context = *memory;
    build->memory = &build->context;
    build->input = module;
    build->control = control;
    effect_summary_defaults(build);
    build->input = NULL;
    status = build->status;
    if (status != XR_XIR_OK) { xir_effect_control_builder_free(build); return status; }
    *output = build;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effect_control_finish(XirControlBuilder *build,
    const XrXirModule *module, EffectControlSummary **output) {
    if (!build || build->active || build->finished || !module || !module->function_count ||
        !module->functions || !output || *output) return XR_XIR_BAD_STRUCTURE;
    if (build->status != XR_XIR_OK) return build->status;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (f >= build->function_capacity || build->function_nodes[f] == UINT32_MAX ||
            !build->graph.nodes[build->function_nodes[f]].collected) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t f = module->function_count; f < build->function_capacity; ++f)
        if (build->function_nodes[f] != UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *memory = build->memory;
    EffectControlSummary *summary = xir_compile_calloc(memory, 1, sizeof(*summary), &build->status);
    if (!summary) return build->status;
    atomic_init(&summary->references, 1);
    build->summary = summary;
    summary->resources = memory->resources;
    summary->functions = module->function_count;
    summary->facts = xir_compile_calloc(memory, summary->functions, sizeof(*summary->facts), &build->status);
    summary->causes = xir_compile_calloc(memory, summary->functions, sizeof(*summary->causes), &build->status);
    summary->task_creation = xir_compile_calloc(memory, summary->functions,
        sizeof(*summary->task_creation), &build->status);
    build->input = module;
    if (build->status == XR_XIR_OK && effect_summary_link(build) && effect_summary_close(build))
        effect_summary_causes(build);
    build->input = NULL;
    if (build->status != XR_XIR_OK) return build->status;
    build->finished = true;
    *output = summary;
    build->summary = NULL;
    return XR_XIR_OK;
}

static XrXirStatus effect_control_summary_build_from(const XrXirCompileContext *memory,
    const XrXirModule *module, XrCompileRequestControl *control,
    const EffectSummaryDispatchSource *dispatch, EffectControlSummary **output) {
    if (!output || *output) return XR_XIR_BAD_STRUCTURE;
    XirControlBuilder *build = NULL;
    XrXirStatus status = xir_effect_control_builder_new(memory, module, control, &build);
    if (status == XR_XIR_OK && dispatch) build->dispatch_source = *dispatch;
    for (uint32_t f = 0; status == XR_XIR_OK && f < module->function_count; ++f) {
        status = xir_effect_control_begin(build, module, f);
        for (uint32_t i = 0; status == XR_XIR_OK && i < module->functions[f].instruction_count; ++i)
            status = xir_effect_control_instruction(build, module, f, i);
        if (status == XR_XIR_OK) status = xir_effect_control_end(build, module, f);
    }
    if (status == XR_XIR_OK) status = xir_effect_control_finish(build, module, output);
    xir_effect_control_builder_free(build);
    return status;
}

static XrXirStatus effect_control_summary_build(const XrXirCompileContext *memory,
    const XrXirModule *module, XrCompileRequestControl *control, EffectControlSummary **output) {
    return effect_control_summary_build_from(memory, module, control, NULL, output);
}

static XrXirStatus effect_control_summary_recheck(const XrXirCompileContext *memory,
    const XrXirModule *module, const EffectControlSummary *known) {
    if (!XR_XIR_PARANOID) return XR_XIR_OK;
    if (!known || known->functions != module->function_count) return XR_XIR_BAD_STRUCTURE;
    EffectControlSummary *fresh = NULL;
    XrXirStatus status = effect_control_summary_build(memory, module, NULL, &fresh);
    for (uint32_t f = 0; status == XR_XIR_OK && f < known->functions; ++f) {
        const XrXirEffectWitness *a = &known->causes[f], *b = &fresh->causes[f];
        if (known->facts[f].suspend != fresh->facts[f].suspend ||
            known->facts[f].throws != fresh->facts[f].throws ||
            known->task_creation[f] != fresh->task_creation[f] || a->cause != b->cause ||
            a->instruction != b->instruction || a->callee != b->callee || a->distance != b->distance)
            status = XR_XIR_BAD_STRUCTURE;
    }
    xir_effect_control_free(fresh);
    return status;
}
