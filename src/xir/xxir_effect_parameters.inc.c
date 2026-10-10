/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_parameters.inc.c - Physical parameter uses on verified value flow
 *
 * KEY CONCEPT:
 *   Captures carry origins, so an escaping closure fixes its original formals.
 *   Actual instructions determine use; stored writer modes grant no authority.
 */

typedef struct EffectParameterFlow {
    const XrXirCompileContext *work;
    const XrXirModule *module;
    XrXirEffects *effects;
    const XrXirFunction *function;
    XrXirEffectParameter *parameters;
    uint64_t *rows;
    uint32_t *previous_uses, *masks;
    uint8_t *conditional, *terms;
    uint32_t words, values;
    bool changed, formula_phase, capture_masks;
} EffectParameterFlow;

static const uint8_t effect_parameter_operands[XR_XIR_OP_COUNT] = {
    0,
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) operands,
#include "xxir_ops.def"
#undef XR_XIR_OP
};

static void effect_parameters_free(XrXirEffects *effects) {
    effect_invocation_certificate_free(effects->invocations);effects->invocations=NULL;
    xr_xir_compile_cell_provenance_free(effects->cells);effects->cells=NULL;
    xr_compile_resources_free(effects->formula_terminals);
    effects->formula_terminals=NULL;
    xr_compile_resources_free(effects->constant_witnesses);
    effects->constant_witnesses=NULL;
    if (!effects->contracts) return;
    for (uint32_t f = 0; f < effects->count; ++f) {
        xr_compile_resources_free((void *)effects->contracts[f].parameters);
        xr_compile_resources_free((void *)effects->contracts[f].formula.terms);
        xr_compile_resources_free((void *)effects->contracts[f].values);
        xr_compile_resources_free((void *)effects->contracts[f].bindings);
    }
    xr_compile_resources_free(effects->contracts);
    effects->contracts = NULL;
}

XR_FUNC const XrXirFunctionEffectContract *xir_effects_contract(
    const XrXirEffects *effects, uint32_t function) {
    return effects && effects->contracts && function < effects->count ?
        &effects->contracts[function] : NULL;
}

/* INSTANCE use roles follow the same actual instruction fixed point.
 * Its complete source/body/domain proof precedes this verified-only
 * analysis hook; a serialized kind or role never admits execution. */
static XrXirStatus effect_parameters_variable_domain(const XrXirModule *module,
    const XrXirCompileContext *work,bool *output) {
    if (!module || !output) return XR_XIR_BAD_STRUCTURE;
    const XrXirProvenance *provenance=module->provenance;
    bool variables=provenance && provenance->kind==XR_XIR_EVIDENCE_TEMPLATE;
    if (provenance && provenance->kind==XR_XIR_EVIDENCE_INSTANCE) {
        XrXirStatus status=effect_contract_instance(work,module,provenance);
        if (status!=XR_XIR_OK) return status;
        variables=true;
    }
    *output=variables;return XR_XIR_OK;
}

static XrXirStatus effect_parameters_allocate(const XrXirModule *module,
    XrXirEffects *effects, const XrXirCompileContext *work) {
    bool variables=false,cells=false;
    XrXirStatus domain=effect_parameters_variable_domain(module,work,&variables);
    if (domain!=XR_XIR_OK) return domain;
    for (uint32_t t=0;module->types && t<module->types->count;++t) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (module->types->nodes[t].kind==XR_XIR_TYPE_CELL) cells=true;
    }
    effect_parameters_free(effects);
    XrXirStatus status = XR_XIR_OK;
    effects->contracts = xir_compile_calloc(work, effects->count,
        sizeof(*effects->contracts), &status);
    if (!effects->contracts) return status;
    uint64_t terminal_count=(uint64_t)effects->count*4;
    if (terminal_count>SIZE_MAX/sizeof(*effects->formula_terminals)) return XR_XIR_BUDGET;
    effects->formula_terminals=xir_compile_calloc(work,(size_t)terminal_count,
        sizeof(*effects->formula_terminals),&status);
    if (!effects->formula_terminals) return status;
    effects->constant_witnesses=xir_compile_calloc(work,(size_t)effects->count*2,
        sizeof(*effects->constant_witnesses),&status);
    if (!effects->constant_witnesses) return status;
    for (uint32_t f = 0; f < effects->count; ++f) {
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
        uint32_t count = module->functions[f].parameter_count;
        XrXirFunctionEffectContract *contract = &effects->contracts[f];
        contract->parameter_count = count;
        if (count) {
            contract->parameters = xir_compile_calloc(work, count,
                sizeof(*contract->parameters), &status);
            if (!contract->parameters) return status;
        }
        XrXirEffectParameter *parameters = (XrXirEffectParameter *)contract->parameters;
        for (uint32_t p = 0; p < count; ++p) {
            if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
            parameters[p].kind = variables && xr_xir_callable_signature(module->types,
                module->functions[f].parameters[p]) ? XR_XIR_EFFECT_PARAMETER_VARIABLE :
                XR_XIR_EFFECT_PARAMETER_FIXED;
        }
    }
    if (cells) return xr_xir_compile_cell_provenance_verified(work,module,&effects->cells,NULL);
    return XR_XIR_OK;
}

static XrXirStatus effect_parameter_join(EffectParameterFlow *flow,
    uint32_t destination, uint32_t source) {
    if (destination >= flow->values || source >= flow->values) return XR_XIR_BAD_STRUCTURE;
    uint64_t *to = flow->rows + (size_t)destination * flow->words;
    const uint64_t *from = flow->rows + (size_t)source * flow->words;
    for (uint32_t w = 0; w < flow->words; ++w) {
        if (!xir_compile_work(flow->work, 3)) return XR_XIR_BUDGET;
        uint64_t joined = to[w] | from[w];
        if (joined != to[w]) flow->changed = true;
        to[w] = joined;
    }
    if (flow->formula_phase) {
        if (!xir_compile_work(flow->work, 3)) return XR_XIR_BUDGET;
        uint32_t mask = flow->masks[destination] | flow->masks[source];
        if (mask != flow->masks[destination]) flow->changed = true;
        flow->masks[destination] = mask;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_parameter_use(EffectParameterFlow *flow,
    uint32_t source, uint32_t use, bool conditional) {
    if (source >= flow->values) return XR_XIR_BAD_STRUCTURE;
    if (flow->formula_phase) return XR_XIR_OK;
    const uint64_t *from = flow->rows + (size_t)source * flow->words;
    for (uint32_t p = 0; p < flow->function->parameter_count; ++p) {
        if (!xir_compile_work(flow->work, 1)) return XR_XIR_BUDGET;
        if (!(from[p / 64] & (UINT64_C(1) << (p % 64)))) continue;
        if (!xir_compile_work(flow->work, 2)) return XR_XIR_BUDGET;
        uint32_t joined = flow->parameters[p].uses | use;
        if (joined != flow->parameters[p].uses) flow->changed = true;
        flow->parameters[p].uses = joined;
        if (conditional) {
            if (!xir_compile_work(flow->work, 1)) return XR_XIR_BUDGET;
            flow->conditional[p] = 1;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_parameter_transfer(EffectParameterFlow *flow,
    uint32_t destination, uint32_t source, uint32_t use) {
    XrXirStatus status = effect_parameter_use(flow, source, use, false);
    return status == XR_XIR_OK ? effect_parameter_join(flow, destination, source) : status;
}

static XrXirStatus effect_parameter_call(EffectParameterFlow *flow,
    const XrXirInstruction *op, bool capture) {
    if (op->immediate < 0 || (uint64_t)op->immediate >= flow->effects->count)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t target = (uint32_t)op->immediate;
    const XrXirFunctionEffectContract *callee = &flow->effects->contracts[target];
    uint32_t destination = flow->function->parameter_count +
        (uint32_t)(op - flow->function->instructions);
    if (op->args[1] > callee->parameter_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t a = 0; a < op->args[1]; ++a) {
        if (!xir_compile_work(flow->work, 2)) return XR_XIR_BUDGET;
        uint32_t value = flow->function->operands[op->args[0] + a];
        XrXirEffectParameter parameter = callee->parameters[a];
        uint32_t use = parameter.kind == XR_XIR_EFFECT_PARAMETER_FIXED ?
            XR_XIR_EFFECT_USE_FIXED : XR_XIR_EFFECT_USE_FORWARD;
        if (capture) use = XR_XIR_EFFECT_USE_CONST_CAPTURE | parameter.uses |
            (parameter.kind == XR_XIR_EFFECT_PARAMETER_FIXED ? XR_XIR_EFFECT_USE_FIXED : 0);
        XrXirStatus status = effect_parameter_use(flow, value, use,
            parameter.kind == XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC);
        if (status == XR_XIR_OK && capture)
            status = effect_parameter_join(flow, destination, value);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_parameter_group(EffectParameterFlow *flow,
    const XrXirInstruction *op, uint32_t use, bool carry) {
    uint32_t first = op->op == XR_XIR_PHI ? 1 : 0;
    uint32_t step = op->op == XR_XIR_PHI ? 2 : 1;
    uint32_t destination = flow->function->parameter_count +
        (uint32_t)(op - flow->function->instructions);
    for (uint32_t a = first; a < op->args[1]; a += step) {
        if (!xir_compile_work(flow->work, 1)) return XR_XIR_BUDGET;
        uint32_t value = flow->function->operands[op->args[0] + a];
        XrXirStatus status = effect_parameter_use(flow, value, use, false);
        if (status == XR_XIR_OK && carry) status = effect_parameter_join(flow, destination, value);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_parameter_opaque(EffectParameterFlow *flow,
    const XrXirInstruction *op, uint32_t use) {
    if (xr_xir_op_uses_operand_table(op->op)) return effect_parameter_group(flow, op, use, false);
    uint32_t count = effect_parameter_operands[op->op];
    if (op->op == XR_XIR_RETURN) count = flow->function->result != XR_XIR_UNIT;
    if (op->op == XR_XIR_SLOT_INIT || op->op == XR_XIR_SLOT_STORE)
        count = xr_xir_slot_payload_operands(flow->module->declarations->slots,
            flow->module->declarations->slot_count, op);
    if (op->op == XR_XIR_CELL_NEW || op->op == XR_XIR_CELL_READ ||
        op->op == XR_XIR_CELL_WRITE || op->op == XR_XIR_CELL_LOCAL_WRITE) {
        XrXirType owner_type=op->op==XR_XIR_CELL_NEW ? op->type :
            xr_xir_operand_type(flow->function,op->args[0]);
        count=xr_xir_cell_payload_operands(flow->module->types,owner_type,op->op);
    }
    for (uint32_t a = 0; a < count; ++a) {
        if (!xir_compile_work(flow->work, 1)) return XR_XIR_BUDGET;
        XrXirStatus status = effect_parameter_use(flow, op->args[a], use, false);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_parameter_instruction(EffectParameterFlow *flow,
    const XrXirInstruction *op) {
    if (!xir_compile_work(flow->work, 1)) return XR_XIR_BUDGET;
    if (op->op <= XR_XIR_INVALID || op->op >= XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    uint32_t destination = flow->function->parameter_count +
        (uint32_t)(op - flow->function->instructions);
    switch (op->op) {
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_NEW:
    case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ:
        return effect_parameter_transfer(flow, destination, op->args[0], XR_XIR_EFFECT_USE_COPY);
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_WRITE:
        return effect_parameter_transfer(flow, op->args[0], op->args[1], XR_XIR_EFFECT_USE_COPY);
    case XR_XIR_PHI:
        return effect_parameter_group(flow, op, XR_XIR_EFFECT_USE_COPY, true);
    case XR_XIR_FUNCTION_REF:
        return effect_parameter_call(flow, op, true);
    case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_CLEANUP_REGISTER:
        return effect_parameter_call(flow, op, false);
    case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT: {
        if (op->immediate < 0 || (uint64_t)op->immediate >= flow->values) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status = effect_parameter_use(flow, (uint32_t)op->immediate,
            XR_XIR_EFFECT_USE_INVOKE, false);
        return status == XR_XIR_OK ? effect_parameter_opaque(flow, op, XR_XIR_EFFECT_USE_OTHER) : status;
    }
    case XR_XIR_FUNCTION_WEAKEN:
        return effect_parameter_use(flow, op->args[0], XR_XIR_EFFECT_USE_FIXED, false);
    case XR_XIR_RETURN:
        return effect_parameter_opaque(flow, op, XR_XIR_EFFECT_USE_RETURN);
    case XR_XIR_GO:
        return effect_parameter_opaque(flow, op, XR_XIR_EFFECT_USE_CHILD);
    case XR_XIR_CALL_REQUIREMENT:
        for (uint32_t a = 0; a < op->args[1]; ++a) {
            if (!xir_compile_work(flow->work, 1)) return XR_XIR_BUDGET;
            XrXirStatus status = effect_parameter_use(flow,
                flow->function->operands[op->args[0] + a], XR_XIR_EFFECT_USE_FORWARD, true);
            if (status != XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    case XR_XIR_CELL_PROJECT:
        /* A scoped descriptor contains no copied callable payload. */
        return XR_XIR_OK;
    case XR_XIR_CELL_NEW:
        if (xr_xir_cell_is_unit(flow->module->types,op->type)) return XR_XIR_OK;
        return effect_parameter_transfer(flow, destination, op->args[0], XR_XIR_EFFECT_USE_STORE);
    case XR_XIR_NULLABLE_SOME:
        return effect_parameter_transfer(flow, destination, op->args[0], XR_XIR_EFFECT_USE_STORE);
    case XR_XIR_CELL_READ: case XR_XIR_NULLABLE_UNWRAP: case XR_XIR_PLACE_READ:
    case XR_XIR_STRUCT_GET: case XR_XIR_CLASS_GET: case XR_XIR_ENUM_GET: case XR_XIR_TUPLE_FIELD:
        return effect_parameter_transfer(flow, destination, op->args[0], XR_XIR_EFFECT_USE_COPY);
    case XR_XIR_CELL_WRITE: case XR_XIR_CELL_LOCAL_WRITE:
        if (xr_xir_cell_is_unit(flow->module->types,xr_xir_operand_type(flow->function,op->args[0])))
            return XR_XIR_OK;
        return effect_parameter_transfer(flow, op->args[0], op->args[1], XR_XIR_EFFECT_USE_STORE);
    case XR_XIR_PLACE_WRITE:
        return effect_parameter_transfer(flow, op->args[0], op->args[1], XR_XIR_EFFECT_USE_STORE);
    case XR_XIR_ARRAY_NEW: case XR_XIR_STRUCT_NEW: case XR_XIR_CLASS_NEW:
    case XR_XIR_ENUM_NEW: case XR_XIR_TUPLE_NEW:
        return effect_parameter_group(flow, op, XR_XIR_EFFECT_USE_STORE, true);
    case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_GROUP_INIT: case XR_XIR_SLOT_STORE:
    case XR_XIR_ARRAY_SET: case XR_XIR_ARRAY_PUSH: case XR_XIR_STRUCT_SET: case XR_XIR_CLASS_SET:
        return effect_parameter_opaque(flow, op, XR_XIR_EFFECT_USE_STORE);
    default:
        return effect_parameter_opaque(flow, op, XR_XIR_EFFECT_USE_OTHER);
    }
}

static XrXirStatus effect_parameter_rows(EffectParameterFlow *flow, EffectGraph *graph) {
    uint64_t values = (uint64_t)flow->function->parameter_count + flow->function->instruction_count;
    uint64_t words = ((uint64_t)flow->function->parameter_count + 63) / 64;
    if (!words && flow->formula_phase) words = 1;
    uint64_t tail = (uint64_t)flow->function->parameter_count * (sizeof(uint32_t) + sizeof(uint8_t));
    if (flow->formula_phase) tail += values * (sizeof(uint32_t) + sizeof(uint8_t));
    if (values > UINT32_MAX || !words || words > UINT32_MAX || tail > SIZE_MAX ||
        values > (SIZE_MAX - tail) / sizeof(uint64_t) / words)
        return XR_XIR_BUDGET;
    flow->words = (uint32_t)words; flow->values = (uint32_t)values;
    size_t bytes = (size_t)values * (size_t)words * sizeof(uint64_t) + (size_t)tail;
    if (!xir_compile_work(flow->work, 2)) return XR_XIR_BUDGET;
    if (bytes > graph->flow_capacity) {
        /* Old contents carry no facts across functions or solver passes. */
        xr_compile_resources_free(graph->flow_rows);
        graph->flow_rows = NULL; graph->flow_capacity = 0;
        XrXirStatus status = XR_XIR_OK;
        graph->flow_rows = xir_compile_calloc(flow->work, 1, bytes, &status);
        if (!graph->flow_rows) return status;
        graph->flow_capacity = bytes;
    } else {
        if (!xir_compile_work(flow->work, bytes)) return XR_XIR_BUDGET;
        memset(graph->flow_rows, 0, bytes);
    }
    flow->rows = graph->flow_rows;
    uint32_t *tail_words = (uint32_t *)(flow->rows + (size_t)values * flow->words);
    flow->previous_uses = tail_words;
    flow->masks = flow->formula_phase ? tail_words + flow->function->parameter_count : NULL;
    flow->conditional = (uint8_t *)(tail_words + flow->function->parameter_count +
        (flow->formula_phase ? (size_t)values : 0));
    flow->terms = flow->formula_phase ? flow->conditional + flow->function->parameter_count : NULL;
    for (uint32_t p = 0; p < flow->function->parameter_count; ++p) {
        if (!xir_compile_work(flow->work, 3)) return XR_XIR_BUDGET;
        const XrXirTypeNode *signature = xr_xir_callable_signature(flow->module->types,flow->function->parameters[p]);
        bool variable = !flow->formula_phase ||
            flow->parameters[p].kind != XR_XIR_EFFECT_PARAMETER_FIXED;
        if (signature && variable)
            flow->rows[(size_t)p * flow->words + p / 64] = UINT64_C(1) << (p % 64);
        if (flow->formula_phase) {
            if (!xir_compile_work(flow->work, 1)) return XR_XIR_BUDGET;
            flow->masks[p] = signature && flow->parameters[p].kind!=XR_XIR_EFFECT_PARAMETER_VARIABLE ? signature->flags &
                (XR_XIR_CALLABLE_ROOT_REQUIRED | XR_XIR_CALLABLE_ROOT_UNRESOLVED) : 0;
        } else {
            flow->previous_uses[p] = flow->parameters[p].uses;
            flow->parameters[p].uses = 0;
        }
    }
    if (flow->formula_phase) for (uint32_t i = 0; i < flow->function->instruction_count; ++i) {
        if (!xir_compile_work(flow->work, 2)) return XR_XIR_BUDGET;
        const XrXirInstruction *op = &flow->function->instructions[i];
        const XrXirTypeNode *signature = xr_xir_callable_signature(flow->module->types,op->type);
        bool transfer = op->op == XR_XIR_COPY || op->op == XR_XIR_PHI ||
            op->op == XR_XIR_LOCAL_NEW || op->op == XR_XIR_LOCAL_READ ||
            op->op == XR_XIR_SCALAR_COPY || op->op == XR_XIR_OWNED_RETAIN ||
            op->op == XR_XIR_SCALAR_LOCAL_NEW || op->op == XR_XIR_SCALAR_LOCAL_READ ||
            op->op == XR_XIR_OWNED_LOCAL_NEW || op->op == XR_XIR_OWNED_LOCAL_READ;
        flow->masks[flow->function->parameter_count + i] = signature && !transfer && op->op != XR_XIR_FUNCTION_REF ?
            signature->flags & (XR_XIR_CALLABLE_ROOT_REQUIRED | XR_XIR_CALLABLE_ROOT_UNRESOLVED) : 0;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_parameter_function(const XrXirModule *module,
    XrXirEffects *effects, uint32_t function, EffectGraph *graph,
    const XrXirCompileContext *work, bool *changed) {
    EffectParameterFlow flow = {0};
    flow.work = work; flow.module = module; flow.effects = effects;
    flow.function = &module->functions[function];
    flow.parameters = (XrXirEffectParameter *)effects->contracts[function].parameters;
    bool has_callable = false;
    for (uint32_t p = 0; p < flow.function->parameter_count; ++p) {
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
        if (xr_xir_callable_signature(module->types, flow.function->parameters[p])) has_callable = true;
    }
    if (!has_callable) return XR_XIR_OK;
    XrXirStatus status = effect_parameter_rows(&flow, graph);
    flow.changed = true;
    while (status == XR_XIR_OK && flow.changed) {
        flow.changed = false;
        for (uint32_t i = 0; status == XR_XIR_OK && i < flow.function->instruction_count; ++i)
            status = effect_parameter_instruction(&flow, &flow.function->instructions[i]);
    }
    for (uint32_t p = 0; status == XR_XIR_OK && p < flow.function->parameter_count; ++p) {
        if (!xir_compile_work(work, 3)) { status = XR_XIR_BUDGET; break; }
        XrXirEffectParameter *parameter = &flow.parameters[p];
        if (!xr_xir_callable_signature(module->types, flow.function->parameters[p])) continue;
        uint32_t forbidden = XR_XIR_EFFECT_USE_FIXED | XR_XIR_EFFECT_USE_STORE |
            XR_XIR_EFFECT_USE_RETURN | XR_XIR_EFFECT_USE_CHILD | XR_XIR_EFFECT_USE_OTHER;
        uint32_t kind = parameter->uses & forbidden ? XR_XIR_EFFECT_PARAMETER_FIXED :
            flow.conditional[p] ? XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC : XR_XIR_EFFECT_PARAMETER_VARIABLE;
        if (parameter->kind != kind || flow.previous_uses[p] != parameter->uses) *changed = true;
        parameter->kind = kind;
    }
    return status;
}

static XrXirStatus effect_parameters_derive(const XrXirModule *module,
    XrXirEffects *effects, EffectGraph *graph, const XrXirCompileContext *work) {
    XrXirStatus status = effect_parameters_allocate(module, effects, work);
    if (status != XR_XIR_OK || !effects->contracts) return status;
    if (!module->provenance || (module->provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE &&
        module->provenance->kind!=XR_XIR_EVIDENCE_INSTANCE)) return XR_XIR_OK;
    bool changed = true;
    while (status == XR_XIR_OK && changed) {
        changed = false;
        for (uint32_t f = 0; status == XR_XIR_OK && f < effects->count; ++f)
            status = effect_parameter_function(module, effects, f, graph, work, &changed);
    }
    return status;
}

/* A charged structural probe retained for the original negative controls.
 * It does not derive a formula or authorize execution. */
static inline XrXirStatus effect_formula_needs_value_flow(EffectParameterFlow *flow,bool *needed) {
    *needed=true;
    for (uint32_t p=0;p<flow->function->parameter_count;++p) {
        if (!xir_compile_work(flow->work,1)) return XR_XIR_BUDGET;
        if (xr_xir_callable_signature(flow->module->types,flow->function->parameters[p]))
            return XR_XIR_OK;
    }
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(flow->work,1)) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&flow->function->instructions[i];
        if (xr_xir_callable_signature(flow->module->types,op->type)) return XR_XIR_OK;
        switch (op->op) {
        case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_CALL_DEFAULT:
        case XR_XIR_INVOKE_DEFAULT: case XR_XIR_CALL_REQUIREMENT:
        case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT:
        case XR_XIR_FUNCTION_REF: case XR_XIR_FUNCTION_WEAKEN:
        case XR_XIR_CLEANUP_REGISTER: case XR_XIR_GO:
            return XR_XIR_OK;
        default: break;
        }
    }
    *needed=false;return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_parameters_match(const XrXirCompileContext *work,
    const XrXirModule *module, const XrXirEffects *effects) {
    if (!xir_compile_context_valid(work) || !module || !effects ||
        effects->resources != work->resources || effects->count != module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    if (!module->provenance || module->provenance->kind != XR_XIR_EVIDENCE_TEMPLATE)
        return XR_XIR_OK;
    if (!effects->contracts || !module->provenance->contracts ||
        module->provenance->contract_count != effects->count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f = 0; f < effects->count; ++f) {
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
        const XrXirFunctionEffectContract *derived = &effects->contracts[f];
        const XrXirFunctionEffectContract *stored = &module->provenance->contracts[f];
        if (derived->parameter_count != stored->parameter_count) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p = 0; p < derived->parameter_count; ++p) {
            if (!xir_compile_work(work, 4)) return XR_XIR_BUDGET;
            if (derived->parameters[p].kind != stored->parameters[p].kind ||
                derived->parameters[p].uses != stored->parameters[p].uses) return XR_XIR_BAD_TYPE;
        }
        if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
        if (derived->formula.constant_mask != stored->formula.constant_mask ||
            derived->formula.term_count != stored->formula.term_count) return XR_XIR_BAD_TYPE;
        for (uint32_t t = 0; t < derived->formula.term_count; ++t) {
            if (!xir_compile_work(work, 4)) return XR_XIR_BUDGET;
            if (derived->formula.terms[t].kind != stored->formula.terms[t].kind ||
                derived->formula.terms[t].index != stored->formula.terms[t].index) return XR_XIR_BAD_TYPE;
        }
        for (uint32_t b = 0; b < stored->binding_count; ++b) {
            if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
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
            const XrXirFunctionEffectContract *callee = &effects->contracts[call->immediate];
            if (binding->parameter >= callee->parameter_count ||
                callee->parameters[binding->parameter].kind == XR_XIR_EFFECT_PARAMETER_FIXED)
                return XR_XIR_BAD_TYPE;
        }
    }
    return XR_XIR_OK;
}
