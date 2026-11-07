/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_summary.inc.c - Module-owned control dependencies and SCC closure
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
    uint32_t head, module, function, instruction;
    uint32_t terminals[2];
    uint8_t local, closed, kind;
} EffectSummaryNode;

typedef struct EffectSummaryEdge {
    uint32_t caller, target, next, instruction;
} EffectSummaryEdge;

typedef struct EffectSummaryDispatch {
    XrXirType receiver;
    uint32_t slot, node;
} EffectSummaryDispatch;

struct EffectControlSummary {
    XrCompileResources *resources;
    uint32_t functions, modules;
    uint32_t node_count, node_capacity, edge_count, edge_capacity;
    uint32_t dispatch_count, dispatch_capacity;
    EffectSummaryNode *nodes;
    EffectSummaryEdge *edges;
    EffectSummaryDispatch *dispatches;
    XrXirFunctionEffects *facts;
    XrXirEffectWitness *causes;
    XrXirEffect *task_creation;
};

typedef struct EffectSummaryBuild {
    const XrXirCompileContext *memory;
    const XrXirModule *input;
    XrCompileRequestControl *control;
    EffectControlSummary *summary;
    uint32_t *dispatch_index;
    size_t *default_offsets;
    uint32_t *default_targets;
    uint32_t dispatch_index_capacity, progress;
    XrXirStatus status;
} EffectSummaryBuild;

static void effect_control_summary_free(EffectControlSummary *summary) {
    if (!summary) return;
    xr_compile_resources_free(summary->nodes);
    xr_compile_resources_free(summary->edges);
    xr_compile_resources_free(summary->dispatches);
    xr_compile_resources_free(summary->facts);
    xr_compile_resources_free(summary->causes);
    xr_compile_resources_free(summary->task_creation);
    xr_compile_resources_free(summary);
}

static bool effect_summary_poll(EffectSummaryBuild *build) {
    if (build->status == XR_XIR_OK)
        build->status = xir_receive_step(build->control, &build->progress);
    return build->status == XR_XIR_OK;
}

static void *effect_summary_grow(EffectSummaryBuild *build, void *old,
    uint32_t count, size_t element, uint32_t *capacity) {
    uint64_t next = *capacity ? (uint64_t)*capacity * 2 : 32;
    if (next > UINT32_MAX) next = UINT32_MAX;
    if (next <= count || next > SIZE_MAX / element) {
        build->status = XR_XIR_OVERFLOW;
        return NULL;
    }
    void *memory = xir_compile_alloc(build->memory, (size_t)next * element, &build->status);
    if (!memory) return NULL;
    if (count) memcpy(memory, old, (size_t)count * element);
    xr_compile_resources_free(old);
    *capacity = (uint32_t)next;
    return memory;
}

static uint32_t effect_summary_node(EffectSummaryBuild *build, uint32_t function,
    uint32_t instruction, EffectSummaryNodeKind kind) {
    EffectControlSummary *summary = build->summary;
    if (summary->node_count == summary->node_capacity) {
        EffectSummaryNode *nodes = effect_summary_grow(build, summary->nodes,
            summary->node_count, sizeof(*nodes), &summary->node_capacity);
        if (!nodes) return UINT32_MAX;
        summary->nodes = nodes;
    }
    uint32_t index = summary->node_count++;
    uint32_t module = build->input->declarations && build->input->declarations->functions ?
        build->input->declarations->functions[function].module : 0;
    if (module >= summary->modules) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return UINT32_MAX;
    }
    summary->nodes[index] = (EffectSummaryNode){.head = UINT32_MAX, .module = module,
        .function = function, .instruction = instruction, .terminals = {UINT32_MAX, UINT32_MAX},
        .kind = (uint8_t)kind};
    return index;
}

static bool effect_summary_edge(EffectSummaryBuild *build, uint32_t caller,
    uint32_t target, uint32_t instruction) {
    EffectControlSummary *summary = build->summary;
    if (caller >= summary->node_count || target >= summary->node_count) {
        build->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    if (summary->edge_count == summary->edge_capacity) {
        EffectSummaryEdge *edges = effect_summary_grow(build, summary->edges,
            summary->edge_count, sizeof(*edges), &summary->edge_capacity);
        if (!edges) return false;
        summary->edges = edges;
    }
    uint32_t index = summary->edge_count++;
    summary->edges[index] = (EffectSummaryEdge){caller, target,
        summary->nodes[caller].head, instruction};
    summary->nodes[caller].head = index;
    return true;
}

static void effect_summary_seed(EffectControlSummary *summary, uint32_t node,
    uint8_t mask, uint32_t instruction) {
    EffectSummaryNode *row = &summary->nodes[node];
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
    EffectControlSummary *summary = build->summary;
    if (build->dispatch_index_capacity &&
        summary->dispatch_count < build->dispatch_index_capacity / 2) return true;
    uint64_t capacity = build->dispatch_index_capacity ?
        (uint64_t)build->dispatch_index_capacity * 2 : 64;
    if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof(uint32_t)) {
        build->status = XR_XIR_OVERFLOW;
        return false;
    }
    uint32_t *index = xir_compile_calloc(build->memory, (size_t)capacity,
        sizeof(*index), &build->status);
    if (!index) return false;
    for (uint32_t d = 0; d < summary->dispatch_count; ++d) {
        const EffectSummaryDispatch *entry = &summary->dispatches[d];
        uint32_t at = effect_summary_dispatch_hash(entry->receiver, entry->slot) & ((uint32_t)capacity - 1);
        while (index[at]) at = (at + 1) & ((uint32_t)capacity - 1);
        index[at] = d + 1;
    }
    xr_compile_resources_free(build->dispatch_index);
    build->dispatch_index = index;
    build->dispatch_index_capacity = (uint32_t)capacity;
    return true;
}

static uint32_t effect_summary_dispatch(EffectSummaryBuild *build, uint32_t function,
    XrXirType receiver, uint32_t slot) {
    if (!effect_summary_dispatch_index(build)) return UINT32_MAX;
    EffectControlSummary *summary = build->summary;
    uint32_t mask = build->dispatch_index_capacity - 1;
    uint32_t at = effect_summary_dispatch_hash(receiver, slot) & mask;
    while (build->dispatch_index[at]) {
        const EffectSummaryDispatch *entry = &summary->dispatches[build->dispatch_index[at] - 1];
        if (entry->receiver == receiver && entry->slot == slot) return entry->node;
        at = (at + 1) & mask;
    }
    if (summary->dispatch_count == summary->dispatch_capacity) {
        EffectSummaryDispatch *entries = effect_summary_grow(build, summary->dispatches,
            summary->dispatch_count, sizeof(*entries), &summary->dispatch_capacity);
        if (!entries) return UINT32_MAX;
        summary->dispatches = entries;
    }
    uint32_t node = effect_summary_node(build, function, UINT32_MAX, EFFECT_SUMMARY_DISPATCH);
    if (node == UINT32_MAX) return node;
    summary->dispatches[summary->dispatch_count] = (EffectSummaryDispatch){receiver, slot, node};
    build->dispatch_index[at] = ++summary->dispatch_count;
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
        values[value] = effect_summary_node(build, function,
            value < body->parameter_count ? UINT32_MAX : value - body->parameter_count,
            EFFECT_SUMMARY_VALUE);
        if (values[value] != UINT32_MAX &&
            !xr_xir_type_is_callable(build->input->types, xr_xir_operand_type(body, value)))
            effect_summary_seed(build->summary, values[value], EFFECT_SUMMARY_UNKNOWN, UINT32_MAX);
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
        uint32_t dispatch = effect_summary_dispatch(build, function, receiver, (uint32_t)op->immediate);
        return dispatch != UINT32_MAX && effect_summary_edge(build, node, dispatch, instruction);
    }
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ:
    case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_READ:
    case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ:
        return effect_summary_value_edge(build, function, node, op->args[0], values, instruction);
    case XR_XIR_LOCAL_UNINIT:
        effect_summary_seed(build->summary, node, EFFECT_SUMMARY_UNKNOWN, instruction);
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
        effect_summary_seed(build->summary, node, EFFECT_SUMMARY_UNKNOWN, instruction);
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
    XrXirFunctionEffects seed = {0};
    EffectSeedProjection projection = effect_seed_opcode(op->op, &seed);
    if (projection == EFFECT_SEED_INVALID) { build->status = XR_XIR_BAD_STRUCTURE; return false; }
    if (seed.suspend == XR_XIR_EFFECT_MAY)
        effect_summary_seed(build->summary, function, EFFECT_SUMMARY_SUSPEND, instruction);
    if (xr_xir_op_is_spawn(op->op))
        effect_summary_seed(build->summary, function, EFFECT_SUMMARY_SPAWN, instruction);
    if (xr_xir_type_is_callable(build->input->types, op->type) &&
        !effect_summary_value_flow(build, function, instruction, values)) return false;
    if (op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE) {
        if (op->immediate < 0 || (uint64_t)op->immediate >= build->input->function_count) {
            build->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
        return effect_summary_edge(build, function, (uint32_t)op->immediate, instruction);
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
        return effect_summary_edge(build, function, target, instruction);
    }
    if (op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT)
        return effect_summary_value_edge(build, function, function,
            (uint32_t)op->immediate, values, instruction);
    if (op->op == XR_XIR_CALL_REQUIREMENT) {
        if (!effect_seed_type(build->input->types, XR_XIR_UNIT, op, &seed)) {
            build->status = XR_XIR_BAD_TYPE;
            return false;
        }
        if (seed.suspend == XR_XIR_EFFECT_UNKNOWN)
            effect_summary_seed(build->summary, function, EFFECT_SUMMARY_UNKNOWN, instruction);
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

static bool effect_summary_function(EffectSummaryBuild *build, uint32_t function) {
    const XrXirModule *module = build->input;
    const XrXirFunction *body = &module->functions[function];
    uint64_t count = (uint64_t)body->parameter_count + body->instruction_count;
    if (count > UINT32_MAX || count > SIZE_MAX / sizeof(uint32_t)) {
        build->status = XR_XIR_OVERFLOW;
        return false;
    }
    uint32_t *values = count ? xir_compile_alloc(build->memory,
        (size_t)count * sizeof(*values), &build->status) : NULL;
    if (count && !values) return false;
    for (uint32_t i = 0; i < (uint32_t)count; ++i) values[i] = UINT32_MAX;
    for (uint32_t p = 0; p < body->parameter_count && build->status == XR_XIR_OK; ++p) {
        const XrXirTypeNode *signature = xr_xir_callable_signature(module->types, body->parameters[p]);
        if (!signature) continue;
        uint32_t node = effect_summary_value(build, function, p, values);
        if (node == UINT32_MAX) break;
        if (!(signature->flags & XR_XIR_CALLABLE_NO_SUSPEND))
            effect_summary_seed(build->summary, node, EFFECT_SUMMARY_UNKNOWN, UINT32_MAX);
    }
    if (module->declarations && module->declarations->functions &&
        xir_sys_pipe_can_suspend(module->declarations->functions[function].native_callable))
        effect_summary_seed(build->summary, function, EFFECT_SUMMARY_SUSPEND, UINT32_MAX);
    for (uint32_t i = 0; i < body->instruction_count && build->status == XR_XIR_OK; ++i)
        if (!effect_summary_instruction(build, function, i, values)) {
            if (build->status == XR_XIR_OK) build->status = XR_XIR_BAD_STRUCTURE;
            break;
        }
    xr_compile_resources_free(values);
    XrXirType error = body->error_type;
    if (module->declarations && module->declarations->functions &&
        (module->declarations->functions[function].promises & XR_XIR_FUNCTION_GENERATOR)) {
        const XrXirTypeNode *iterator = xr_xir_type_node(module->types, body->result);
        if (!iterator || iterator->kind != XR_XIR_TYPE_ITERATOR) build->status = XR_XIR_BAD_TYPE;
        else error = iterator->result;
    }
    if (error == XR_XIR_UNIT) build->status = XR_XIR_BAD_TYPE;
    build->summary->facts[function].throws = error == XR_XIR_NEVER ? XR_XIR_EFFECT_NONE : XR_XIR_EFFECT_MAY;
    return build->status == XR_XIR_OK;
}

typedef struct EffectSummaryCandidate {
    EffectSummaryBuild *build;
    uint32_t dispatch;
} EffectSummaryCandidate;

static XrXirStatus effect_summary_candidate(void *owner, const XirClassMethodTarget *target) {
    EffectSummaryCandidate *candidate = owner;
    return effect_summary_edge(candidate->build, candidate->dispatch, target->function,
        UINT32_MAX) ? XR_XIR_OK : candidate->build->status;
}

static bool effect_summary_link(EffectSummaryBuild *build) {
    EffectControlSummary *summary = build->summary;
    for (uint32_t d = 0; d < summary->dispatch_count; ++d) {
        if (!effect_summary_poll(build)) return false;
        EffectSummaryDispatch dispatch = summary->dispatches[d];
        if (build->input->linkage_kind != XR_XIR_PROGRAM) {
            effect_summary_seed(summary, dispatch.node, EFFECT_SUMMARY_UNKNOWN, UINT32_MAX);
            continue;
        }
        EffectSummaryCandidate candidate = {build, dispatch.node};
        build->status = xir_compile_class_method_candidates(build->input, dispatch.receiver,
            dispatch.slot, effect_summary_candidate, &candidate, build->control);
        if (build->status != XR_XIR_OK) return false;
    }
    return true;
}

typedef struct EffectSummaryFrame {
    uint32_t node, edge, parent;
} EffectSummaryFrame;

static bool effect_summary_close(EffectSummaryBuild *build) {
    EffectControlSummary *summary = build->summary;
    uint32_t count = summary->node_count;
    uint32_t *index = xir_compile_calloc(build->memory, count, sizeof(*index), &build->status);
    uint32_t *low = xir_compile_calloc(build->memory, count, sizeof(*low), &build->status);
    uint32_t *component = xir_compile_alloc(build->memory, (size_t)count * sizeof(*component), &build->status);
    uint32_t *stack = xir_compile_alloc(build->memory, (size_t)count * sizeof(*stack), &build->status);
    EffectSummaryFrame *frames = xir_compile_calloc(build->memory, count, sizeof(*frames), &build->status);
    if (!index || !low || !component || !stack || !frames) goto done;
    for (uint32_t n = 0; n < count; ++n) component[n] = UINT32_MAX;
    uint32_t next = 0, components = 0, stacked = 0;
    for (uint32_t start = 0; start < count && build->status == XR_XIR_OK; ++start) {
        if (index[start]) continue;
        uint32_t depth = 1;
        frames[0] = (EffectSummaryFrame){start, summary->nodes[start].head, UINT32_MAX};
        index[start] = low[start] = ++next;
        stack[stacked++] = start;
        while (depth && effect_summary_poll(build)) {
            EffectSummaryFrame *frame = &frames[depth - 1];
            uint32_t node = frame->node;
            if (frame->edge != UINT32_MAX) {
                const EffectSummaryEdge *edge = &summary->edges[frame->edge];
                frame->edge = edge->next;
                uint32_t target = edge->target;
                if (!index[target]) {
                    index[target] = low[target] = ++next;
                    stack[stacked++] = target;
                    frames[depth++] = (EffectSummaryFrame){target, summary->nodes[target].head, node};
                } else if (component[target] == UINT32_MAX && index[target] < low[node]) {
                    low[node] = index[target];
                }
                continue;
            }
            uint32_t parent = frame->parent;
            --depth;
            if (low[node] == index[node]) {
                uint32_t end = stacked;
                uint8_t mask = 0;
                do {
                    XR_DCHECK(stacked, "An active SCC owns at least its root");
                    uint32_t member = stack[--stacked];
                    component[member] = components;
                    mask |= summary->nodes[member].local;
                } while (stack[stacked] != node);
                for (uint32_t at = stacked; at < end; ++at) {
                    const EffectSummaryNode *member = &summary->nodes[stack[at]];
                    for (uint32_t e = member->head; e != UINT32_MAX; e = summary->edges[e].next) {
                        uint32_t target = summary->edges[e].target;
                        XR_DCHECK(component[target] != UINT32_MAX, "SCC dependencies close before their callers");
                        if (component[target] != components) mask |= summary->nodes[target].closed;
                    }
                }
                for (uint32_t at = stacked; at < end; ++at) summary->nodes[stack[at]].closed = mask;
                ++components;
            }
            if (parent != UINT32_MAX && low[node] < low[parent]) low[parent] = low[node];
        }
    }
done:
    xr_compile_resources_free(index);
    xr_compile_resources_free(low);
    xr_compile_resources_free(component);
    xr_compile_resources_free(stack);
    xr_compile_resources_free(frames);
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
    EffectControlSummary *summary = build->summary;
    uint32_t count = summary->node_count;
    uint32_t *heads = xir_compile_alloc(build->memory, (size_t)count * sizeof(*heads), &build->status);
    uint32_t *queue = xir_compile_alloc(build->memory, (size_t)count * sizeof(*queue), &build->status);
    uint32_t *next = summary->edge_count ? xir_compile_alloc(build->memory,
        (size_t)summary->edge_count * sizeof(*next), &build->status) : NULL;
    EffectSummaryReason *reasons = xir_compile_calloc(build->memory, count, sizeof(*reasons), &build->status);
    if (!heads || !queue || (summary->edge_count && !next) || !reasons) goto done;
    for (uint32_t n = 0; n < count; ++n) heads[n] = UINT32_MAX;
    for (uint32_t e = summary->edge_count; e-- > 0;) {
        uint32_t target = summary->edges[e].target;
        next[e] = heads[target];
        heads[target] = e;
    }
    uint32_t front = 0, back = 0;
    for (uint32_t n = 0; n < count; ++n) {
        const EffectSummaryNode *node = &summary->nodes[n];
        uint8_t suspension = effect_summary_suspension(node->closed);
        if (!suspension || !(node->local & suspension)) continue;
        bool function = node->kind == EFFECT_SUMMARY_FUNCTION;
        reasons[n] = (EffectSummaryReason){function ? n : UINT32_MAX, 0, true};
        if (function) summary->causes[n] = (XrXirEffectWitness){
            suspension == EFFECT_SUMMARY_SUSPEND ? XR_XIR_EFFECT_CAUSE_SUSPEND : XR_XIR_EFFECT_CAUSE_INDIRECT,
            node->terminals[suspension == EFFECT_SUMMARY_SUSPEND], UINT32_MAX, 0};
        queue[back++] = n;
    }
    while (front < back && effect_summary_poll(build)) {
        uint32_t target = queue[front++];
        for (uint32_t e = heads[target]; e != UINT32_MAX; e = next[e]) {
            const EffectSummaryEdge *edge = &summary->edges[e];
            uint32_t caller = edge->caller;
            if (reasons[caller].ready || effect_summary_suspension(summary->nodes[caller].closed) !=
                effect_summary_suspension(summary->nodes[target].closed)) continue;
            EffectSummaryReason reason = reasons[target];
            if (summary->nodes[caller].kind == EFFECT_SUMMARY_FUNCTION) {
                uint32_t distance = reason.function == UINT32_MAX ? 0 : reason.distance + 1;
                summary->causes[caller] = (XrXirEffectWitness){
                    reason.function == UINT32_MAX ? XR_XIR_EFFECT_CAUSE_INDIRECT : XR_XIR_EFFECT_CAUSE_CALL,
                    edge->instruction, reason.function, distance};
                reason.function = caller;
                reason.distance = distance;
            }
            reasons[caller] = reason;
            queue[back++] = caller;
        }
    }
    for (uint32_t f = 0; f < summary->functions && build->status == XR_XIR_OK; ++f) {
        uint8_t suspension = effect_summary_suspension(summary->nodes[f].closed);
        if (suspension && !reasons[f].ready) build->status = XR_XIR_BAD_STRUCTURE;
        summary->facts[f].suspend = suspension == EFFECT_SUMMARY_SUSPEND ? XR_XIR_EFFECT_MAY :
            suspension ? XR_XIR_EFFECT_UNKNOWN : XR_XIR_EFFECT_NONE;
        summary->task_creation[f] = summary->nodes[f].closed & EFFECT_SUMMARY_SPAWN ?
            XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_NONE;
    }
done:
    xr_compile_resources_free(heads);
    xr_compile_resources_free(queue);
    xr_compile_resources_free(next);
    xr_compile_resources_free(reasons);
    return build->status == XR_XIR_OK;
}

static XrXirStatus effect_control_summary_build(const XrXirCompileContext *memory,
    const XrXirModule *module, XrCompileRequestControl *control, EffectControlSummary **output) {
    if (!xir_compile_context_valid(memory) || !module || !module->function_count ||
        !module->functions || !output || *output) return XR_XIR_BAD_STRUCTURE;
    EffectSummaryBuild build = {.memory = memory, .input = module, .control = control};
    EffectControlSummary *summary = xir_compile_calloc(memory, 1, sizeof(*summary), &build.status);
    if (!summary) return build.status;
    build.summary = summary;
    summary->resources = memory->resources;
    summary->functions = module->function_count;
    summary->modules = module->declarations ? module->declarations->module_count : 1;
    summary->facts = xir_compile_calloc(memory, summary->functions, sizeof(*summary->facts), &build.status);
    summary->causes = xir_compile_calloc(memory, summary->functions, sizeof(*summary->causes), &build.status);
    summary->task_creation = xir_compile_calloc(memory, summary->functions,
        sizeof(*summary->task_creation), &build.status);
    for (uint32_t f = 0; f < summary->functions && build.status == XR_XIR_OK; ++f) {
        uint32_t node = effect_summary_node(&build, f, UINT32_MAX, EFFECT_SUMMARY_FUNCTION);
        if (node != f && build.status == XR_XIR_OK) build.status = XR_XIR_BAD_STRUCTURE;
    }
    if (build.status == XR_XIR_OK) effect_summary_defaults(&build);
    for (uint32_t f = 0; f < summary->functions && build.status == XR_XIR_OK; ++f)
        if (!effect_summary_function(&build, f)) break;
    if (build.status == XR_XIR_OK && effect_summary_link(&build) && effect_summary_close(&build))
        effect_summary_causes(&build);
    xr_compile_resources_free(build.dispatch_index);
    xr_compile_resources_free(build.default_offsets);
    xr_compile_resources_free(build.default_targets);
    if (build.status != XR_XIR_OK) {
        effect_control_summary_free(summary);
        return build.status;
    }
    *output = summary;
    return XR_XIR_OK;
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
    effect_control_summary_free(fresh);
    return status;
}
