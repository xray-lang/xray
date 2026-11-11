/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_parameters.inc.c - Callable parameter summaries from direct instruction arrays
 *
 * KEY CONCEPT:
 *   Numeric value dependencies preserve capture and forwarding boundaries.
 *   Two SCC closures derive roles and their exact use masks without body replay.
 */

static const uint8_t effect_parameter_operands[XR_XIR_OP_COUNT] = {
    0,
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) operands,
#include "xxir_ops.def"
#undef XR_XIR_OP
};

XR_FUNC const XrXirFunctionEffectContract *xir_effects_contract(
    const XrXirEffects *effects, uint32_t function) {
    const XrXirFunctionEffectContract *contracts=effect_contracts_read(effects);
    return effects && contracts && function<effects->count?&contracts[function]:NULL;
}

/* INSTANCE use roles follow the same actual instruction fixed point.
 * Its complete source/body/domain proof precedes this verified-only
 * analysis hook; a serialized kind or role never admits execution. */

enum { EFFECT_PARAMETER_CONDITIONAL = 1, EFFECT_PARAMETER_FIXED = 2 };
static const uint32_t effect_parameter_forbidden = XR_XIR_EFFECT_USE_FIXED | XR_XIR_EFFECT_USE_STORE |
    XR_XIR_EFFECT_USE_RETURN | XR_XIR_EFFECT_USE_CHILD | XR_XIR_EFFECT_USE_OTHER;

typedef struct EffectParameterRow { uint32_t first, parameters, instructions; } EffectParameterRow;
typedef struct EffectParameterCall { uint32_t source, target; bool capture; } EffectParameterCall;
typedef struct EffectParameterGraph {
    const XrXirCompileContext *work;
    XrXirModule module;
    XrXirTypes types;
    XrXirDeclarations declarations;
    XrCompileRequestControl *control;
    EffectParameterRow *rows;
    XirEffectSccNode *roles, *uses;
    XirEffectSccEdge *role_edges, *use_edges;
    EffectParameterCall *calls;
    uint32_t count, role_count, role_capacity, use_count, use_capacity, call_count, call_capacity, progress;
    XrXirStatus status;
} EffectParameterGraph;
typedef struct EffectParameterFlow {
    EffectParameterGraph *graph;
    const XrXirModule *module;
    XrXirFunction body;
    const XrXirFunction *function;
    uint32_t function_index, instruction;
} EffectParameterFlow;

static XrXirStatus effect_parameter_poll(EffectParameterGraph *graph) {
    if (graph->status == XR_XIR_OK && graph->control && !(++graph->progress & 255u))
        graph->status = xir_compile_resource_status(xr_compile_request_control_poll(graph->control));
    return graph->status;
}
static uint32_t effect_parameter_node(EffectParameterFlow *flow, uint32_t value) {
    const EffectParameterRow *row = &flow->graph->rows[flow->function_index];
    if (row->first == UINT32_MAX || value >= (uint64_t)row->parameters + row->instructions) {
        flow->graph->status = XR_XIR_BAD_STRUCTURE;
        return UINT32_MAX;
    }
    return row->first + value;
}
static XrXirStatus effect_parameter_type(EffectParameterFlow *flow, uint32_t value, XrXirType *type) {
    if (effect_parameter_node(flow, value) == UINT32_MAX) return flow->graph->status;
    *type = xr_xir_operand_type(flow->function, value);
    return XR_XIR_OK;
}
static XrXirStatus effect_parameter_edge(EffectParameterGraph *graph, uint32_t source,
    uint32_t target, bool usage) {
    if (graph->status != XR_XIR_OK) return graph->status;
    XirEffectSccNode *nodes = usage ? graph->uses : graph->roles;
    XirEffectSccEdge **edges = usage ? &graph->use_edges : &graph->role_edges;
    uint32_t *count = usage ? &graph->use_count : &graph->role_count;
    uint32_t *capacity = usage ? &graph->use_capacity : &graph->role_capacity;
    if (source >= graph->count || target >= graph->count) return XR_XIR_BAD_STRUCTURE;
    if (*count == *capacity) {
        XirEffectSccEdge *fresh = xir_compile_array_grow(graph->work, *edges,
            *count, sizeof(**edges), capacity, &graph->status);
        if (!fresh) return graph->status;
        *edges = fresh;
    }
    (*edges)[*count] = (XirEffectSccEdge){target, nodes[source].head};
    nodes[source].head = (*count)++;
    return XR_XIR_OK;
}
static XrXirStatus effect_parameter_join(EffectParameterFlow *flow, uint32_t destination, uint32_t source) {
    uint32_t from = effect_parameter_node(flow, source), to = effect_parameter_node(flow, destination);
    if (from == UINT32_MAX || to == UINT32_MAX) return flow->graph->status;
    XrXirStatus status = effect_parameter_edge(flow->graph, from, to, false);
    return status == XR_XIR_OK ? effect_parameter_edge(flow->graph, from, to, true) : status;
}
static XrXirStatus effect_parameter_use(EffectParameterFlow *flow,
    uint32_t source, uint32_t use, bool conditional) {
    uint32_t node = effect_parameter_node(flow, source);
    if (node == UINT32_MAX) return flow->graph->status;
    flow->graph->uses[node].seed |= use;
    if (use & effect_parameter_forbidden) flow->graph->roles[node].seed |= EFFECT_PARAMETER_FIXED;
    if (conditional) flow->graph->roles[node].seed |= EFFECT_PARAMETER_CONDITIONAL;
    return XR_XIR_OK;
}
static XrXirStatus effect_parameter_transfer(EffectParameterFlow *flow,
    uint32_t destination, uint32_t source, uint32_t use) {
    XrXirStatus status = effect_parameter_use(flow, source, use, false);
    return status == XR_XIR_OK ? effect_parameter_join(flow, destination, source) : status;
}
/* Role closure precedes use closure. Direct calls then choose FIXED or FORWARD
 * from the final callee role, while captures also inherit its complete uses. */
static XrXirStatus effect_parameter_call(EffectParameterFlow *flow,
    const XrXirInstruction *op, bool capture) {
    EffectParameterGraph *graph = flow->graph;
    if (op->immediate < 0 || (uint64_t)op->immediate >= flow->module->function_count ||
        op->args[0] > flow->function->operand_count ||
        op->args[1] > flow->function->operand_count - op->args[0]) return XR_XIR_BAD_STRUCTURE;
    uint32_t function = (uint32_t)op->immediate;
    const XrXirFunction callee = flow->module->functions[function];
    const EffectParameterRow *row = &graph->rows[function];
    if (callee.parameter_count != row->parameters || op->args[1] > callee.parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t a = 0; a < op->args[1]; ++a) {
        uint32_t value = flow->function->operands[op->args[0] + a];
        uint32_t source = effect_parameter_node(flow, value), target = UINT32_MAX;
        if (source == UINT32_MAX) return graph->status;
        if (xr_xir_callable_signature(flow->module->types, callee.parameters[a])) {
            if (row->first == UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            target = row->first + a;
            XrXirStatus status = effect_parameter_edge(graph, source, target, false);
            if (status == XR_XIR_OK && capture) status = effect_parameter_edge(graph, source, target, true);
            if (status != XR_XIR_OK) return status;
        } else graph->roles[source].seed |= EFFECT_PARAMETER_FIXED;
        if (graph->call_count == graph->call_capacity) {
            EffectParameterCall *fresh = xir_compile_array_grow(graph->work, graph->calls,
                graph->call_count, sizeof(*fresh), &graph->call_capacity, &graph->status);
            if (!fresh) return graph->status;
            graph->calls = fresh;
        }
        graph->calls[graph->call_count++] = (EffectParameterCall){source, target, capture};
        if (capture) {
            XrXirStatus status = effect_parameter_join(flow,
                flow->function->parameter_count + flow->instruction, value);
            if (status != XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_parameter_group(EffectParameterFlow *flow,
    const XrXirInstruction *op, uint32_t use, bool carry) {
    if (op->args[0] > flow->function->operand_count ||
        op->args[1] > flow->function->operand_count - op->args[0]) return XR_XIR_BAD_STRUCTURE;
    uint32_t first = op->op == XR_XIR_PHI || op->op==XR_XIR_SCOPED_GO ? 1 : 0;
    uint32_t step = op->op == XR_XIR_PHI ? 2 : 1;
    uint32_t destination = flow->function->parameter_count +
        flow->instruction;
    for (uint32_t a = first; a < op->args[1]; a += step) {

        uint32_t value = flow->function->operands[op->args[0] + a];
        XrXirStatus status = effect_parameter_use(flow, value, use, false);
        if (status == XR_XIR_OK && carry) status = effect_parameter_join(flow, destination, value);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_parameter_opaque(EffectParameterFlow *flow,
    const XrXirInstruction *op, uint32_t use) {
    if (xr_xir_op_uses_owner_slots(op->op)) return XR_XIR_OK;
    if (op->op==XR_XIR_NULLABLE_SOME && xr_xir_nullable_is_unit(flow->module->types,op->type)) return XR_XIR_OK;
    if (xr_xir_op_uses_operand_table(op->op)) return effect_parameter_group(flow, op, use, false);
    uint32_t count = effect_parameter_operands[op->op];
    if (op->op == XR_XIR_RETURN) count = flow->function->result != XR_XIR_UNIT &&
        !(flow->module->declarations && (flow->module->declarations->functions[flow->function_index].promises & XR_XIR_FUNCTION_GENERATOR));
    if (op->op==XR_XIR_GENERATOR_YIELD)
        count=xr_xir_iterator_element(flow->module->types,flow->function->result)!=XR_XIR_UNIT;
    if (op->op == XR_XIR_SLOT_INIT || op->op == XR_XIR_SLOT_STORE)
        count = xr_xir_slot_payload_operands(flow->module->declarations->slots,
            flow->module->declarations->slot_count, op);
    if (op->op == XR_XIR_CELL_NEW || op->op == XR_XIR_CELL_READ ||
        op->op == XR_XIR_CELL_WRITE || op->op == XR_XIR_CELL_LOCAL_WRITE) {
        XrXirType owner_type=op->type;
        if ((op->op!=XR_XIR_CELL_NEW&&op->op!=XR_XIR_CELL_NEW_TAKE)) {
            XrXirStatus status=effect_parameter_type(flow,op->args[0],&owner_type);
            if (status!=XR_XIR_OK) return status;
        }
        count=xr_xir_cell_payload_operands(flow->module->types,owner_type,op->op);
    }
    for (uint32_t a = 0; a < count; ++a) {

        XrXirStatus status = effect_parameter_use(flow, op->args[a], use, false);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_parameter_instruction(EffectParameterFlow *flow,
    const XrXirInstruction *op) {

    if (op->op <= XR_XIR_INVALID || op->op >= XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    uint32_t destination = flow->function->parameter_count +
        flow->instruction;
    switch (op->op) {
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_NEW:
    case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ:
        return effect_parameter_transfer(flow, destination, op->args[0], XR_XIR_EFFECT_USE_COPY);
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_WRITE:
        return effect_parameter_transfer(flow, op->args[0], op->args[1], XR_XIR_EFFECT_USE_COPY);
    case XR_XIR_PHI:
        return effect_parameter_group(flow, op, XR_XIR_EFFECT_USE_COPY, true);
    case XR_XIR_CLASS_METHOD_REF:
        return effect_parameter_transfer(flow,destination,op->args[0],XR_XIR_EFFECT_USE_CONST_CAPTURE|XR_XIR_EFFECT_USE_OTHER);
    case XR_XIR_FUNCTION_REF:
        return effect_parameter_call(flow, op, true);
    case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_CLEANUP_REGISTER:
        return effect_parameter_call(flow, op, false);
    case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT: {
        if (op->immediate < 0 || (uint64_t)op->immediate >=
            (uint64_t)flow->function->parameter_count + flow->function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status = effect_parameter_use(flow, (uint32_t)op->immediate,
            XR_XIR_EFFECT_USE_INVOKE, false);
        return status == XR_XIR_OK ? effect_parameter_opaque(flow, op, XR_XIR_EFFECT_USE_OTHER) : status;
    }
    case XR_XIR_FUNCTION_WEAKEN:
        return effect_parameter_use(flow, op->args[0], XR_XIR_EFFECT_USE_FIXED, false);
    case XR_XIR_GENERATOR_YIELD:
    case XR_XIR_RETURN:
        return effect_parameter_opaque(flow, op, XR_XIR_EFFECT_USE_RETURN);
    case XR_XIR_GO: case XR_XIR_SCOPED_GO: case XR_XIR_GENERATOR_NEW:
        return effect_parameter_opaque(flow, op, XR_XIR_EFFECT_USE_CHILD);
    case XR_XIR_CALL_REQUIREMENT:
        if (op->args[0] > flow->function->operand_count ||
            op->args[1] > flow->function->operand_count - op->args[0]) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a = 0; a < op->args[1]; ++a) {

            XrXirStatus status = effect_parameter_use(flow,
                flow->function->operands[op->args[0] + a], XR_XIR_EFFECT_USE_FORWARD, true);
            if (status != XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    case XR_XIR_CELL_PROJECT:
        /* A scoped descriptor contains no copied callable payload. */
        return XR_XIR_OK;
    case XR_XIR_CELL_NEW_TAKE: case XR_XIR_CELL_NEW:
        if (xr_xir_cell_is_unit(flow->module->types,op->type)) return XR_XIR_OK;
        return effect_parameter_transfer(flow, destination, op->args[0], XR_XIR_EFFECT_USE_STORE);
    case XR_XIR_NULLABLE_SOME:
        if (xr_xir_nullable_is_unit(flow->module->types,op->type)) return XR_XIR_OK;
        return effect_parameter_transfer(flow, destination, op->args[0], XR_XIR_EFFECT_USE_STORE);
    case XR_XIR_SET_UNION: case XR_XIR_SET_INTERSECTION: case XR_XIR_SET_DIFFERENCE:
    case XR_XIR_SET_SYMMETRIC_DIFFERENCE: {
        XrXirStatus status = effect_parameter_transfer(flow, destination, op->args[0], XR_XIR_EFFECT_USE_COPY);
        return status == XR_XIR_OK ? effect_parameter_transfer(flow, destination, op->args[1], XR_XIR_EFFECT_USE_COPY) : status;
    }
    case XR_XIR_CELL_READ: case XR_XIR_NULLABLE_UNWRAP: case XR_XIR_PLACE_READ:
    case XR_XIR_HASH_KEY_AT: case XR_XIR_MAP_VALUE_AT: case XR_XIR_MAP_GET: case XR_XIR_MAP_INDEX: case XR_XIR_HASH_KEYS: case XR_XIR_HASH_VALUES: case XR_XIR_MAP_ENTRIES: case XR_XIR_STRUCT_GET: case XR_XIR_CLASS_GET: case XR_XIR_ENUM_GET: case XR_XIR_TUPLE_FIELD:
        return effect_parameter_transfer(flow, destination, op->args[0], XR_XIR_EFFECT_USE_COPY);
    case XR_XIR_CELL_WRITE_TAKE: case XR_XIR_CELL_WRITE: case XR_XIR_CELL_LOCAL_WRITE: {
        XrXirType type=XR_XIR_UNIT;XrXirStatus status=effect_parameter_type(flow,op->args[0],&type);
        if (status!=XR_XIR_OK) return status;
        if (xr_xir_cell_is_unit(flow->module->types,type)) return XR_XIR_OK;
        return effect_parameter_transfer(flow, op->args[0], op->args[1], XR_XIR_EFFECT_USE_STORE);
    }
    case XR_XIR_PLACE_WRITE:
        return effect_parameter_transfer(flow, op->args[0], op->args[1], XR_XIR_EFFECT_USE_STORE);
    case XR_XIR_MAP_NEW: case XR_XIR_SET_NEW: case XR_XIR_ARRAY_NEW: case XR_XIR_STRUCT_NEW: case XR_XIR_CLASS_NEW:
    case XR_XIR_ENUM_NEW: case XR_XIR_TUPLE_NEW:
        return effect_parameter_group(flow, op, XR_XIR_EFFECT_USE_STORE, true);
    case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_GROUP_INIT: case XR_XIR_SLOT_STORE:
    case XR_XIR_MAP_SET: case XR_XIR_SET_ADD: case XR_XIR_ARRAY_SET: case XR_XIR_ARRAY_PUSH: case XR_XIR_STRUCT_SET: case XR_XIR_CLASS_SET:
        return effect_parameter_opaque(flow, op, XR_XIR_EFFECT_USE_STORE);
    default:
        return effect_parameter_opaque(flow, op, XR_XIR_EFFECT_USE_OTHER);
    }
}

static void effect_parameter_graph_free(EffectParameterGraph *graph) {
    xr_compile_resources_free(graph->rows);
    xr_compile_resources_free(graph->roles);
    xr_compile_resources_free(graph->uses);
    xr_compile_resources_free(graph->role_edges);
    xr_compile_resources_free(graph->use_edges);
    xr_compile_resources_free(graph->calls);
}
static XrXirStatus effect_parameter_graph_prepare(EffectParameterGraph *graph,
    const XrXirFunctionEffectContract *contracts) {
    uint32_t functions = graph->module.function_count;
    graph->rows = xir_compile_calloc(graph->work, functions, sizeof(*graph->rows), &graph->status);
    if (!graph->rows) return graph->status;
    uint64_t total = 0;
    for (uint32_t f = 0; f < functions; ++f) {
        if (effect_parameter_poll(graph) != XR_XIR_OK) return graph->status;
        const XrXirFunction *body = &graph->module.functions[f];
        if (body->parameter_count != contracts[f].parameter_count) return XR_XIR_BAD_STRUCTURE;
        bool callable = false;
        for (uint32_t p = 0; p < body->parameter_count; ++p)
            callable |= xr_xir_callable_signature(graph->module.types, body->parameters[p]) != NULL;
        uint64_t values = (uint64_t)body->parameter_count + body->instruction_count;
        graph->rows[f] = (EffectParameterRow){callable ? (uint32_t)total : UINT32_MAX,
            body->parameter_count, body->instruction_count};
        if (callable) {
            total += values;
            if (total > UINT32_MAX || total > SIZE_MAX / sizeof(XirEffectSccNode)) return XR_XIR_OVERFLOW;
        }
    }
    graph->count = (uint32_t)total;
    if (!total) return XR_XIR_OK;
    graph->roles = xir_compile_calloc(graph->work, graph->count, sizeof(*graph->roles), &graph->status);
    graph->uses = xir_compile_calloc(graph->work, graph->count, sizeof(*graph->uses), &graph->status);
    if (!graph->roles || !graph->uses) return graph->status;
    for (uint32_t n = 0; n < graph->count; ++n) graph->roles[n].head = graph->uses[n].head = UINT32_MAX;
    return XR_XIR_OK;
}
static XrXirStatus effect_parameter_graph_collect(EffectParameterGraph *graph) {
    for (uint32_t f = 0; f < graph->module.function_count; ++f) {
        if (graph->rows[f].first == UINT32_MAX) continue;
        EffectParameterFlow flow = {.graph = graph, .module = &graph->module, .function_index = f,
            .body = graph->module.functions[f]};
        flow.function = &flow.body;
        if (flow.body.parameter_count != graph->rows[f].parameters ||
            flow.body.instruction_count != graph->rows[f].instructions) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t i = 0; i < flow.body.instruction_count; ++i) {
            if (effect_parameter_poll(graph) != XR_XIR_OK) return graph->status;
            const XrXirInstruction op = flow.body.instructions[i];
            flow.instruction = i;
            XrXirStatus status = effect_parameter_instruction(&flow, &op);
            if (status != XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_parameter_graph_finish(EffectParameterGraph *graph,
    XrXirFunctionEffectContract *contracts) {
    XrXirStatus status = xir_effect_scc_close(graph->work, graph->roles, graph->count,
        graph->role_edges, graph->role_count, graph->control);
    if (status != XR_XIR_OK) return status;
    for (uint32_t c = 0; c < graph->call_count; ++c) {
        const EffectParameterCall *call = &graph->calls[c];
        bool fixed = call->target == UINT32_MAX || (graph->roles[call->target].closed & EFFECT_PARAMETER_FIXED);
        uint32_t use = fixed ? XR_XIR_EFFECT_USE_FIXED : XR_XIR_EFFECT_USE_FORWARD;
        if (call->capture) use = XR_XIR_EFFECT_USE_CONST_CAPTURE | (fixed ? XR_XIR_EFFECT_USE_FIXED : 0);
        graph->uses[call->source].seed |= use;
    }
    status = xir_effect_scc_close(graph->work, graph->uses, graph->count,
        graph->use_edges, graph->use_count, graph->control);
    if (status != XR_XIR_OK) return status;
    for (uint32_t f = 0; f < graph->module.function_count; ++f) {
        const EffectParameterRow *row = &graph->rows[f];
        if (row->first == UINT32_MAX) continue;
        XrXirEffectParameter *parameters = (XrXirEffectParameter *)contracts[f].parameters;
        for (uint32_t p = 0; p < row->parameters; ++p) {
            if (!xr_xir_callable_signature(graph->module.types, graph->module.functions[f].parameters[p])) continue;
            uint32_t node = row->first + p, roles = graph->roles[node].closed;
            parameters[p].kind = roles & EFFECT_PARAMETER_FIXED ? XR_XIR_EFFECT_PARAMETER_FIXED :
                roles & EFFECT_PARAMETER_CONDITIONAL ? XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC : XR_XIR_EFFECT_PARAMETER_VARIABLE;
            parameters[p].uses = graph->uses[node].closed;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_parameters_current(const XrXirCompileContext *work,
    const XrXirModule *module, const XrXirEffects *effects) {
    if (!xir_compile_context_valid(work) || !module || !effects ||
        effects->resources != work->resources || effects->count != module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    if (!module->provenance || module->provenance->kind != XR_XIR_EVIDENCE_TEMPLATE)
        return XR_XIR_OK;
    if (!effect_contracts_read(effects) || !module->provenance->contracts ||
        module->provenance->contract_count != effects->count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f = 0; f < effects->count; ++f) {

        const XrXirFunctionEffectContract *derived = &effect_contracts_read(effects)[f];
        const XrXirFunctionEffectContract *stored = &module->provenance->contracts[f];
        if (derived->parameter_count != stored->parameter_count) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p = 0; p < derived->parameter_count; ++p) {

            if (derived->parameters[p].kind != stored->parameters[p].kind ||
                derived->parameters[p].uses != stored->parameters[p].uses) return XR_XIR_BAD_TYPE;
        }

        if (derived->formula.constant_mask != stored->formula.constant_mask ||
            derived->formula.term_count != stored->formula.term_count) return XR_XIR_BAD_TYPE;
        for (uint32_t t = 0; t < derived->formula.term_count; ++t) {

            if (derived->formula.terms[t].kind != stored->formula.terms[t].kind ||
                derived->formula.terms[t].index != stored->formula.terms[t].index) return XR_XIR_BAD_TYPE;
        }
        for (uint32_t b = 0; b < stored->binding_count; ++b) {

            const XrXirEffectCallBinding *binding = &stored->bindings[b];
            if (binding->family != XR_XIR_EFFECT_BINDING_DIRECT &&
                binding->family != XR_XIR_EFFECT_BINDING_CAPTURE &&
                binding->family != XR_XIR_EFFECT_BINDING_REQUIREMENT) return XR_XIR_BAD_TYPE;
            XrXirStatus status = xir_effect_call_binding_candidate(work,module,f,
                binding->instruction,binding->parameter);
            if (status != XR_XIR_OK) return status;
            const XrXirInstruction *call = &module->functions[f].instructions[binding->instruction];
            if (binding->family==XR_XIR_EFFECT_BINDING_REQUIREMENT) {
                if (call->op!=XR_XIR_CALL_REQUIREMENT) return XR_XIR_BAD_STRUCTURE;
                continue;
            }
            const XrXirFunctionEffectContract *callee = &effect_contracts_read(effects)[call->immediate];
            if (binding->parameter >= callee->parameter_count ||
                !xir_effect_parameter_identity(module->functions[call->immediate].parameters[binding->parameter],
                    (XrXirEffectParameterKind)callee->parameters[binding->parameter].kind))
                return XR_XIR_BAD_TYPE;
        }
    }
    return XR_XIR_OK;
}
