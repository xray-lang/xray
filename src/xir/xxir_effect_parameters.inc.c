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

static XrXirStatus effect_parameters_allocate(const XrXirModule *module,
    XrXirEffects *effects, const XrXirCompileContext *work) {
    bool template=module->provenance && module->provenance->kind==XR_XIR_EVIDENCE_TEMPLATE;
    bool cells=false;
    for (uint32_t t=0;module->types && t<module->types->count;++t) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (module->types->nodes[t].kind==XR_XIR_TYPE_CELL) cells=true;
    }
    if (!template && !cells) return XR_XIR_OK;
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
            parameters[p].kind = template && xr_xir_callable_signature(module->types,
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

static uint32_t effect_cell_root_mask(uint32_t intrinsic) {
    return (intrinsic&XR_XIR_CELL_ACCESS_ROOT?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
        (intrinsic&XR_XIR_CELL_ACCESS_UNKNOWN?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
}

static XrXirStatus effect_formula_edge_mask(const XrXirCompileContext *work,
    const XrXirTypes *types, const XrXirEffects *effects, const XrXirFunction *functions,
    uint32_t caller, const XrXirInstruction *op, uint32_t target, uint32_t *output) {
    if (!effects->contracts || caller>=effects->count || target>=effects->count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirRootFormula *formula=&effects->contracts[target].formula;
    uint32_t mask=formula->constant_mask;
    for (uint32_t t=0;t<formula->term_count;++t) {
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        const XrXirRootTerm *term=&formula->terms[t];
        if (term->kind==XR_XIR_ROOT_TERM_CONTEXT_CALL) mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
        else if (term->kind==XR_XIR_ROOT_TERM_PARAMETER) {
            if (term->index>=functions[target].parameter_count) return XR_XIR_BAD_STRUCTURE;
            const XrXirTypeNode *bound=xr_xir_callable_signature(types,functions[target].parameters[term->index]);
            if (!bound) return XR_XIR_BAD_TYPE;
            mask|=bound->flags&(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        } else if (term->kind==XR_XIR_ROOT_TERM_CELL_PARAMETER) {
            if (term->index>=functions[target].parameter_count ||
                !xr_xir_type_is_cell(types,functions[target].parameters[term->index]))
                return XR_XIR_BAD_STRUCTURE;
            if (term->index>=op->args[1]) { mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;continue; }
            const XrXirFunction *body=&functions[caller];
            if (!body->operands || op->args[0]>body->operand_count || op->args[1]>body->operand_count-op->args[0])
                return XR_XIR_BAD_STRUCTURE;
            uint32_t actual=body->operands[op->args[0]+term->index],cell_mask=0;
            XrXirCellAccessRequest access={caller,actual,NULL,0};
            XrXirStatus status=xr_xir_compile_cell_access(work,effects->cells,&access,&cell_mask);
            if (status!=XR_XIR_OK) return status;
            mask|=effect_cell_root_mask(cell_mask);
        } else return XR_XIR_BAD_STRUCTURE;
    }
    *output=mask;return XR_XIR_OK;
}

/* Cell provenance is a separate domain. Reading a logical callable payload
 * never transfers its physical owner's authority into that callable bound. */
static XrXirStatus effect_formula_cell_origins(EffectParameterFlow *flow,
    uint32_t value, uint32_t destination, uint32_t *mask, XrXirCellOriginView *view) {
    uint32_t function=(uint32_t)(flow->function-flow->module->functions);
    XrXirStatus status=xr_xir_compile_cell_origin_view(flow->work,flow->effects->cells,function,value,view);
    if (status!=XR_XIR_OK) return status;
    *mask|=effect_cell_root_mask(view->intrinsic_mask);
    if (view->word_count!=(view->parameter_count+63u)/64u ||
        (!!view->dependencies!=!!view->word_count) || (!!view->parameters!=!!view->parameter_count))
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t d=0;d<view->parameter_count;++d) {
        if (!xir_compile_work(flow->work,2)) return XR_XIR_BUDGET;
        if (!(view->dependencies[d/64]&(UINT64_C(1)<<(d%64)))) continue;
        uint32_t parameter=view->parameters[d];
        if (parameter>=flow->function->parameter_count ||
            !xr_xir_type_is_cell(flow->module->types,flow->function->parameters[parameter]))
            return XR_XIR_BAD_STRUCTURE;
        if (destination==UINT32_MAX) flow->terms[parameter]=XR_XIR_ROOT_TERM_CELL_PARAMETER;
        else {
            if (destination>=flow->values) return XR_XIR_BAD_VALUE;
            uint64_t *row=flow->rows+(size_t)destination*flow->words;
            uint64_t bit=UINT64_C(1)<<(parameter%64);
            if (!(row[parameter/64]&bit)) { row[parameter/64]|=bit;flow->changed=true; }
        }
    }
    return XR_XIR_OK;
}

/* A latent symbolic dependency is distinct from the closure's advertised
 * bound. Closed Fn8 references retain Fn8; only actual variable origins carry
 * parameters, and a fixed barrier leaves no such origin to recover. */
static XrXirStatus effect_formula_capture(EffectParameterFlow *flow,
    const XrXirInstruction *op, uint32_t destination) {
    const XrXirFunctionEffectContract *callee = &flow->effects->contracts[op->immediate];
    const XrXirTypeNode *signature = xr_xir_callable_signature(flow->module->types,op->type);
    if (!signature) return XR_XIR_BAD_TYPE;
    uint32_t mask = signature->flags &
        (XR_XIR_CALLABLE_ROOT_REQUIRED | XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    if (!flow->module->provenance || flow->module->provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE) {
        if (flow->capture_masks) {
            uint32_t joined=flow->masks[destination]|mask;
            if (joined!=flow->masks[destination]) flow->changed=true;
            flow->masks[destination]=joined;
        }
        return XR_XIR_OK;
    }
    bool symbolic = callee->formula.term_count != 0;
    uint32_t actual_mask = callee->formula.constant_mask;
    for (uint32_t t = 0; t < callee->formula.term_count; ++t) {
        if (!xir_compile_work(flow->work,3)) return XR_XIR_BUDGET;
        const XrXirRootTerm *term = &callee->formula.terms[t];
        if (term->kind==XR_XIR_ROOT_TERM_CELL_PARAMETER) {
            if (term->index>=op->args[1]) { symbolic=false;continue; }
            XrXirCellOriginView view={0};
            XrXirStatus status=effect_formula_cell_origins(flow,
                flow->function->operands[op->args[0]+term->index],destination,&actual_mask,&view);
            if (status!=XR_XIR_OK) return status;
            continue;
        }
        if (term->kind != XR_XIR_ROOT_TERM_PARAMETER || term->index >= op->args[1] ||
            callee->parameters[term->index].kind != XR_XIR_EFFECT_PARAMETER_VARIABLE) {
            symbolic = false; continue;
        }
        uint32_t value = flow->function->operands[op->args[0] + term->index];
        if (value >= flow->values) return XR_XIR_BAD_VALUE;
        const uint64_t *origins = flow->rows + (size_t)value * flow->words;
        bool present = false;
        for (uint32_t w = 0; w < flow->words; ++w) {
            if (!xir_compile_work(flow->work,1)) return XR_XIR_BUDGET;
            if (origins[w]) present = true;
        }
        if (!present) symbolic = false;
        if (!xir_compile_work(flow->work,1)) return XR_XIR_BUDGET;
        actual_mask |= flow->masks[value];
        XrXirStatus status = effect_parameter_join(flow,destination,value);
        if (status != XR_XIR_OK) return status;
    }
    if (!flow->capture_masks) return XR_XIR_OK;
    if (symbolic) mask = (mask & XR_XIR_CALLABLE_ROOT_REQUIRED) | actual_mask;
    if (!xir_compile_work(flow->work,3)) return XR_XIR_BUDGET;
    uint32_t joined = flow->masks[destination] | mask;
    if (joined != flow->masks[destination]) flow->changed = true;
    flow->masks[destination] = joined;
    return XR_XIR_OK;
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
    if (capture && flow->formula_phase) return effect_formula_capture(flow,op,destination);
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
    if (!module->provenance || module->provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE)
        return XR_XIR_OK;
    bool changed = true;
    while (status == XR_XIR_OK && changed) {
        changed = false;
        for (uint32_t f = 0; status == XR_XIR_OK && f < effects->count; ++f)
            status = effect_parameter_function(module, effects, f, graph, work, &changed);
    }
    return status;
}

static XrXirStatus effect_formula_witness_start(EffectParameterFlow *flow) {
    uint32_t f=(uint32_t)(flow->function-flow->module->functions);
    if (!flow->effects->formula_terminals || !flow->effects->constant_witnesses) return XR_XIR_BAD_STRUCTURE;
    for (unsigned unknown=0;unknown<2;++unknown) {
        for (unsigned constant=0;constant<2;++constant) {
            if (!xir_compile_work(flow->work,sizeof(XrXirRootEffectWitness))) return XR_XIR_BUDGET;
            memset(&flow->effects->formula_terminals[((size_t)constant*2+unknown)*flow->effects->count+f],0,
                sizeof(XrXirRootEffectWitness));
        }
        XrXirRootEffectWitness *constant=&flow->effects->constant_witnesses[(size_t)unknown*flow->effects->count+f];
        if (!xir_compile_work(flow->work,sizeof(*constant))) return XR_XIR_BUDGET;
        memset(constant,0,sizeof(*constant));
        XrXirRootEffectWitness *to=unknown?&flow->effects->unresolved_witnesses[f]:
            &flow->effects->root_witnesses[f];
        if (!xir_compile_work(flow->work,1)) return XR_XIR_BUDGET;
        bool authentic_local=!unknown && (to->cause==XR_XIR_ROOT_CAUSE_MUTABLE_SLOT ||
            to->cause==XR_XIR_ROOT_CAUSE_NON_SENDABLE_CONST || to->cause==XR_XIR_ROOT_CAUSE_INITIALIZER ||
            to->cause==XR_XIR_ROOT_CAUSE_REQUIREMENT);
        if (authentic_local) {
            if (!xir_compile_work(flow->work,5)) return XR_XIR_BUDGET;
            *constant=*to;continue;
        }
        if (!to->cause) continue;
        if (!xir_compile_work(flow->work,sizeof(*to))) return XR_XIR_BUDGET;
        memset(to,0,sizeof(*to));
    }
    return XR_XIR_OK;
}

/* Called only with actual instruction/value contributions reconstructed by
 * this owner. The certificate is a value copy, never a producer borrow. */
static XrXirStatus effect_formula_witness_add(EffectParameterFlow *flow,
    uint32_t mask, XrXirRootEffectWitness candidate, bool constant) {
    uint32_t f=(uint32_t)(flow->function-flow->module->functions);
    bool initializer=candidate.cause==XR_XIR_ROOT_CAUSE_INITIALIZER && candidate.instruction==UINT32_MAX;
    if ((!initializer && candidate.instruction>=flow->function->instruction_count) || candidate.distance)
        return XR_XIR_BAD_STRUCTURE;
    for (unsigned unknown=0;unknown<2;++unknown) {
        if (!xir_compile_work(flow->work,1)) return XR_XIR_BUDGET;
        uint32_t bit=unknown?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED;
        if (!(mask&bit)) continue;
        for (unsigned domain=0;domain<=(constant?1u:0u);++domain) {
            XrXirRootEffectWitness *to=domain?
                &flow->effects->constant_witnesses[(size_t)unknown*flow->effects->count+f]:
                unknown?&flow->effects->unresolved_witnesses[f]:&flow->effects->root_witnesses[f];
            if (!xir_compile_work(flow->work,5)) return XR_XIR_BUDGET;
            if (to->cause && !effect_root_cause_before(&candidate,to)) continue;
            bool certified=candidate.cause==XR_XIR_ROOT_CAUSE_PARAMETER ||
                candidate.cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL ||
                candidate.cause==XR_XIR_ROOT_CAUSE_CELL_ACCESS ||
                candidate.cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER;
            if (!xir_compile_work(flow->work,certified?10:5)) return XR_XIR_BUDGET;
            *to=candidate;
            if (certified)
                flow->effects->formula_terminals[((size_t)domain*2+unknown)*flow->effects->count+f]=candidate;
        }
    }
    return XR_XIR_OK;
}

/* The caller supplies a physical operand, not a metadata-selected callback.
 * Registration authenticates a potential exit execution with its real owner. */
static XrXirStatus effect_formula_execution(EffectParameterFlow *flow,
    uint32_t instruction, uint32_t value, uint32_t callee, uint32_t physical) {
    if (!xir_compile_work(flow->work,6)) return XR_XIR_BUDGET;
    if (instruction>=flow->function->instruction_count || value>=flow->values)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&flow->function->instructions[instruction];
    if (callee==UINT32_MAX)
        return (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) &&
            op->immediate>=0 && (uint64_t)op->immediate==value && physical==UINT32_MAX ?
            XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    uint32_t target=UINT32_MAX;
    if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_CLEANUP_REGISTER) {
        if (op->immediate>=0 && (uint64_t)op->immediate<flow->effects->count)
            target=(uint32_t)op->immediate;
    } else if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
        const uint32_t *identity=xr_xir_default_identity(op);
        const XrXirDefaultBinding *binding=NULL;
        if (!identity) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=xr_xir_compile_default_lookup(flow->work,
            flow->module,identity[0],identity[1],&binding);
        if (status!=XR_XIR_OK) return status;
        if (binding) target=binding->function;
    } else return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->work,8)) return XR_XIR_BUDGET;
    if (target!=callee || target>=flow->effects->count || target>=flow->module->function_count ||
        physical>=op->args[1] || op->args[0]>flow->function->operand_count ||
        op->args[1]>flow->function->operand_count-op->args[0] || !flow->function->operands)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionEffectContract *contract=&flow->effects->contracts[target];
    if (physical>=contract->parameter_count || !contract->parameters ||
        contract->parameters[physical].kind!=XR_XIR_EFFECT_PARAMETER_VARIABLE ||
        flow->function->operands[op->args[0]+physical]!=value)
        return XR_XIR_BAD_STRUCTURE;
    if (op->op==XR_XIR_CLEANUP_REGISTER) {
        const XrXirDeclarations *d=flow->module->declarations;
        uint32_t owner=(uint32_t)(flow->function-flow->module->functions);
        if (!xir_compile_work(flow->work,4)) return XR_XIR_BUDGET;
        if (!d || !d->functions || d->functions[target].cleanup_owner!=owner+1 ||
            flow->module->functions[target].parameter_count!=op->args[1])
            return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t t=0;t<contract->formula.term_count;++t) {
        if (!xir_compile_work(flow->work,2)) return XR_XIR_BUDGET;
        if (contract->formula.terms[t].kind==XR_XIR_ROOT_TERM_PARAMETER &&
            contract->formula.terms[t].index==physical) return XR_XIR_OK;
    }
    return XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_formula_witness_value(EffectParameterFlow *flow,
    uint32_t instruction, uint32_t value, uint32_t callee, uint32_t physical) {
    XrXirStatus status=effect_formula_execution(flow,instruction,value,callee,physical);
    if (status!=XR_XIR_OK) return status;
    const uint64_t *origins=flow->rows+(size_t)value*flow->words;
    for (uint32_t p=0;p<flow->function->parameter_count;++p) {
        if (!xir_compile_work(flow->work,2)) return XR_XIR_BUDGET;
        if (!(origins[p/64]&(UINT64_C(1)<<(p%64)))) continue;
        if (xr_xir_type_is_cell(flow->module->types,flow->function->parameters[p])) {
            XrXirRootEffectWitness terminal={XR_XIR_ROOT_CAUSE_CELL_PARAMETER,instruction,UINT32_MAX,p,0};
            status=effect_formula_witness_add(flow,XR_XIR_CALLABLE_ROOT_UNRESOLVED,terminal,false);
            if (status!=XR_XIR_OK) return status;
            continue;
        }
        if (flow->parameters[p].kind!=XR_XIR_EFFECT_PARAMETER_VARIABLE) {
            XrXirRootEffectWitness context={XR_XIR_ROOT_CAUSE_CONTEXT_CALL,instruction,UINT32_MAX,UINT32_MAX,0};
            if (!xir_compile_work(flow->work,1)) return XR_XIR_BUDGET;
            bool constant=!effect_contract_context_op(flow->function->instructions[instruction].op);
            status=effect_formula_witness_add(flow,XR_XIR_CALLABLE_ROOT_UNRESOLVED,context,constant);
            if (status!=XR_XIR_OK) return status;
            continue;
        }
        const XrXirTypeNode *signature=xr_xir_callable_signature(flow->module->types,flow->function->parameters[p]);
        if (!signature) return XR_XIR_BAD_TYPE;
        XrXirRootEffectWitness terminal={XR_XIR_ROOT_CAUSE_PARAMETER,instruction,UINT32_MAX,p,0};
        status=effect_formula_witness_add(flow,signature->flags,terminal,false);
        if (status!=XR_XIR_OK) return status;
    }
    if (!xir_compile_work(flow->work,1)) return XR_XIR_BUDGET;
    XrXirRootEffectWitness terminal={callee==UINT32_MAX?XR_XIR_ROOT_CAUSE_INDIRECT:
        XR_XIR_ROOT_CAUSE_PARAMETER,instruction,callee,callee==UINT32_MAX?UINT32_MAX:physical,0};
    return effect_formula_witness_add(flow,flow->masks[value],terminal,true);
}

static XrXirStatus effect_formula_context_witness(EffectParameterFlow *flow, uint32_t instruction) {
    if (!xir_compile_work(flow->work,3)) return XR_XIR_BUDGET;
    if (instruction>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&flow->function->instructions[instruction];
    if (!effect_contract_context_op(op->op) && op->op!=XR_XIR_CLEANUP_REGISTER &&
        op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT)
        return XR_XIR_BAD_STRUCTURE;
    XrXirRootEffectWitness terminal={XR_XIR_ROOT_CAUSE_CONTEXT_CALL,instruction,UINT32_MAX,UINT32_MAX,0};
    return effect_formula_witness_add(flow,XR_XIR_CALLABLE_ROOT_UNRESOLVED,terminal,
        op->op==XR_XIR_CLEANUP_REGISTER);
}

static XrXirStatus effect_formula_value(EffectParameterFlow *flow,
    uint32_t instruction, uint32_t value, uint32_t *constant) {
    if (value >= flow->values) return XR_XIR_BAD_VALUE;
    if (!xir_compile_work(flow->work, 2)) return XR_XIR_BUDGET;
    *constant |= flow->masks[value];
    const uint64_t *origins = flow->rows + (size_t)value * flow->words;
    for (uint32_t p = 0; p < flow->function->parameter_count; ++p) {
        if (!xir_compile_work(flow->work, 1)) return XR_XIR_BUDGET;
        if (!(origins[p / 64] & (UINT64_C(1) << (p % 64)))) continue;
        if (!xir_compile_work(flow->work, 1)) return XR_XIR_BUDGET;
        if (xr_xir_type_is_cell(flow->module->types,flow->function->parameters[p]))
            flow->terms[p]=XR_XIR_ROOT_TERM_CELL_PARAMETER;
        else if (flow->parameters[p].kind==XR_XIR_EFFECT_PARAMETER_VARIABLE) flow->terms[p] = 1;
        else if (effect_contract_context_op(flow->function->instructions[instruction].op))
            flow->terms[flow->function->parameter_count+instruction]=1;
        else *constant|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_formula_cell_use(EffectParameterFlow *flow,
    uint32_t instruction, uint32_t value, uint32_t *constant) {
    XrXirCellOriginView view={0};uint32_t mask=0;
    XrXirStatus status=effect_formula_cell_origins(flow,value,UINT32_MAX,&mask,&view);
    if (status!=XR_XIR_OK) return status;
    *constant|=mask;
    XrXirRootEffectWitness access={XR_XIR_ROOT_CAUSE_CELL_ACCESS,instruction,UINT32_MAX,value,0};
    status=effect_formula_witness_add(flow,mask,access,true);
    for (uint32_t d=0;d<view.parameter_count && status==XR_XIR_OK;++d) {
        if (!xir_compile_work(flow->work,2)) return XR_XIR_BUDGET;
        if (!(view.dependencies[d/64]&(UINT64_C(1)<<(d%64)))) continue;
        XrXirRootEffectWitness parameter={XR_XIR_ROOT_CAUSE_CELL_PARAMETER,instruction,
            UINT32_MAX,view.parameters[d],0};
        status=effect_formula_witness_add(flow,XR_XIR_CALLABLE_ROOT_UNRESOLVED,parameter,false);
    }
    return status;
}

static XrXirStatus effect_formula_call(EffectParameterFlow *flow,
    uint32_t instruction, uint32_t *constant) {
    const XrXirInstruction *op = &flow->function->instructions[instruction];
    uint32_t target = (uint32_t)op->immediate;
    bool default_call=op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT;
    if (default_call) {
        const XrXirDefaultBinding *binding = NULL;
        const uint32_t *identity = xr_xir_default_identity(op);
        XrXirStatus status = xr_xir_compile_default_lookup(flow->work,
            flow->module,identity[0],identity[1],&binding);
        if (status != XR_XIR_OK) return status;
        if (!binding) return XR_XIR_BAD_STRUCTURE;
        target = binding->function;
    }
    if (!xir_compile_work(flow->work,5)) return XR_XIR_BUDGET;
    if (target >= flow->effects->count || target>=flow->module->function_count ||
        (!default_call && (op->args[0]>flow->function->operand_count ||
        op->args[1]>flow->function->operand_count-op->args[0] ||
        (!!op->args[1] && !flow->function->operands)))) return XR_XIR_BAD_STRUCTURE;
    if (default_call && flow->module->functions[target].parameter_count) return XR_XIR_BAD_STRUCTURE;
    if (op->op==XR_XIR_CLEANUP_REGISTER) {
        const XrXirDeclarations *d=flow->module->declarations;
        uint32_t owner=(uint32_t)(flow->function-flow->module->functions);
        if (!xir_compile_work(flow->work,4)) return XR_XIR_BUDGET;
        if (!d || !d->functions || d->functions[target].cleanup_owner!=owner+1 ||
            flow->module->functions[target].parameter_count!=op->args[1])
            return XR_XIR_BAD_STRUCTURE;
    }
    const XrXirRootFormula *from = &flow->effects->contracts[target].formula;
    if (!xir_compile_work(flow->work, 2)) return XR_XIR_BUDGET;
    *constant |= from->constant_mask;
    for (uint32_t t = 0; t < from->term_count; ++t) {
        if (!xir_compile_work(flow->work, 2)) return XR_XIR_BUDGET;
        const XrXirRootTerm *term = &from->terms[t];
        if (term->kind == XR_XIR_ROOT_TERM_CONTEXT_CALL) {
            if (effect_contract_context_op(op->op)) {
                if (!xir_compile_work(flow->work, 1)) return XR_XIR_BUDGET;
                flow->terms[flow->function->parameter_count + instruction] = 1;
            } else *constant |= XR_XIR_CALLABLE_ROOT_UNRESOLVED;
            XrXirStatus status=effect_formula_context_witness(flow,instruction);
            if (status!=XR_XIR_OK) return status;
        } else if (term->kind == XR_XIR_ROOT_TERM_CELL_PARAMETER) {
            if (default_call || term->index>=op->args[1] ||
                term->index>=flow->module->functions[target].parameter_count ||
                !xr_xir_type_is_cell(flow->module->types,flow->module->functions[target].parameters[term->index]))
                return XR_XIR_BAD_STRUCTURE;
            XrXirStatus status=effect_formula_cell_use(flow,instruction,
                flow->function->operands[op->args[0]+term->index],constant);
            if (status!=XR_XIR_OK) return status;
        } else if (term->kind == XR_XIR_ROOT_TERM_PARAMETER) {
            if (default_call || term->index >= op->args[1] || term->index>=flow->effects->contracts[target].parameter_count ||
                flow->effects->contracts[target].parameters[term->index].kind!=XR_XIR_EFFECT_PARAMETER_VARIABLE)
                return XR_XIR_BAD_STRUCTURE;
            uint32_t value = flow->function->operands[op->args[0] + term->index];
            XrXirStatus status = effect_formula_value(flow,instruction,value,constant);
            if (status==XR_XIR_OK) status=effect_formula_witness_value(flow,instruction,value,target,term->index);
            if (status != XR_XIR_OK) return status;
        } else return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_formula_slot(EffectParameterFlow *flow,
    uint32_t instruction, uint32_t *constant) {
    const XrXirInstruction *op = &flow->function->instructions[instruction];
    if (!flow->module->declarations || op->immediate < 0 ||
        (uint64_t)op->immediate >= flow->module->declarations->slot_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirSlot *slot = &flow->module->declarations->slots[op->immediate];
    if (!xir_compile_work(flow->work, 1)) return XR_XIR_BUDGET;
    XrXirRootEffectCause cause=XR_XIR_ROOT_CAUSE_MUTABLE_SLOT;
    if (!slot->mutable) {
        uint32_t function = (uint32_t)(flow->function - flow->module->functions);
        XrXirProofContext proof = {flow->module,{XR_XIR_CONTEXT_FUNCTION,function,0}};
        XrXirStatus status = xr_xir_compile_type_markers_prove(flow->work,
            &proof,slot->type,XR_XIR_CONSTRAINT_SENDABLE);
        if (status == XR_XIR_OK) return XR_XIR_OK;
        if (status != XR_XIR_BAD_TYPE) return status;
        cause=XR_XIR_ROOT_CAUSE_NON_SENDABLE_CONST;
    }
    *constant |= XR_XIR_CALLABLE_ROOT_REQUIRED;
    XrXirRootEffectWitness witness={cause,instruction,UINT32_MAX,(uint32_t)op->immediate,0};
    return effect_formula_witness_add(flow,XR_XIR_CALLABLE_ROOT_REQUIRED,witness,true);
}

static XrXirStatus effect_formula_instruction(EffectParameterFlow *flow,
    uint32_t instruction, uint32_t *constant) {
    if (!xir_compile_work(flow->work, 1)) return XR_XIR_BUDGET;
    const XrXirInstruction *op = &flow->function->instructions[instruction];
    switch (op->op) {
    case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_CALL_DEFAULT:
    case XR_XIR_INVOKE_DEFAULT: case XR_XIR_CLEANUP_REGISTER:
        return effect_formula_call(flow,instruction,constant);
    case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT:
        if (op->immediate < 0 || (uint64_t)op->immediate >= flow->values)
            return XR_XIR_BAD_VALUE;
        {
            XrXirStatus status=effect_formula_value(flow,instruction,(uint32_t)op->immediate,constant);
            if (status==XR_XIR_OK) status=effect_formula_witness_value(flow,instruction,
                (uint32_t)op->immediate,UINT32_MAX,UINT32_MAX);
            return status;
        }
    case XR_XIR_CALL_REQUIREMENT: {
        const XrXirInterfaceTable *table = flow->module->types ? flow->module->types->interfaces : NULL;
        if (!table || op->targets[0] >= table->count ||
            op->targets[1] >= table->declarations[op->targets[0]].method_count)
            return XR_XIR_BAD_STRUCTURE;
        const XrXirTypeNode *signature = xr_xir_callable_signature(flow->module->types,
            table->declarations[op->targets[0]].methods[op->targets[1]].signature);
        if (!signature) return XR_XIR_BAD_TYPE;
        if (!xir_compile_work(flow->work, 2)) return XR_XIR_BUDGET;
        *constant |= signature->flags & XR_XIR_CALLABLE_ROOT_REQUIRED;
        if (signature->flags&XR_XIR_CALLABLE_ROOT_REQUIRED) {
            XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_REQUIREMENT,instruction,UINT32_MAX,UINT32_MAX,0};
            XrXirStatus status=effect_formula_witness_add(flow,XR_XIR_CALLABLE_ROOT_REQUIRED,witness,true);
            if (status!=XR_XIR_OK) return status;
        }
        if (signature->flags & XR_XIR_CALLABLE_ROOT_UNRESOLVED) {
            flow->terms[flow->function->parameter_count + instruction] = 1;
            return effect_formula_context_witness(flow,instruction);
        }
        return XR_XIR_OK;
    }
    case XR_XIR_SLOT_LOAD: case XR_XIR_SLOT_STORE: case XR_XIR_SLOT_PLACE:
        return effect_formula_slot(flow,instruction,constant);
    case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE: case XR_XIR_CELL_LOCAL_WRITE:
        return effect_formula_cell_use(flow,instruction,op->args[0],constant);
    case XR_XIR_PLACE_READ: case XR_XIR_PLACE_WRITE:
        if (flow->effects->cells) return effect_formula_cell_use(flow,instruction,op->args[0],constant);
        return XR_XIR_OK;
    case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_GROUP_INIT: {
        uint32_t slot=op->op==XR_XIR_SLOT_GROUP_INIT?
            (uint32_t)((uint64_t)op->immediate>>32):(uint32_t)op->immediate;
        XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_INITIALIZER,instruction,UINT32_MAX,slot,0};
        *constant |= XR_XIR_CALLABLE_ROOT_REQUIRED;
        return effect_formula_witness_add(flow,XR_XIR_CALLABLE_ROOT_REQUIRED,witness,true);
    }
    default:
        /* Seed already exhaustively classifies every opcode; construction,
         * return and GO transport do not execute the transported callable. */
        return XR_XIR_OK;
    }
}

static XrXirRootTerm effect_formula_term(const EffectParameterFlow *flow, uint32_t value) {
    uint32_t parameters=flow->function->parameter_count;
    return (XrXirRootTerm){value<parameters ?
        (xr_xir_type_is_cell(flow->module->types,flow->function->parameters[value]) ?
            XR_XIR_ROOT_TERM_CELL_PARAMETER : XR_XIR_ROOT_TERM_PARAMETER) : XR_XIR_ROOT_TERM_CONTEXT_CALL,
        value<parameters ? value : value-parameters};
}

static XrXirStatus effect_formula_publish(EffectParameterFlow *flow,
    uint32_t function, uint32_t constant, bool *changed) {
    uint32_t count=0;
    for (uint32_t v=0;v<flow->values;++v) {
        if (!xir_compile_work(flow->work,1)) return XR_XIR_BUDGET;
        if (flow->terms[v]) ++count;
    }
    XrXirRootFormula *to=&flow->effects->contracts[function].formula;
    bool different=to->constant_mask!=constant || to->term_count!=count;
    /* The counted empty set has no ordering or records to compare. Its
     * constant still carries every genuine ROOT and UNKNOWN contribution. */
    if (!count) {
        if (!different) return XR_XIR_OK;
        if (!xir_compile_work(flow->work,3)) return XR_XIR_BUDGET;
        xr_compile_resources_free((void *)to->terms);
        *to=(XrXirRootFormula){constant,0,NULL};*changed=true;return XR_XIR_OK;
    }
    uint32_t at=0;
    for (uint32_t kind=XR_XIR_ROOT_TERM_PARAMETER;kind<=XR_XIR_ROOT_TERM_CELL_PARAMETER;++kind) {
        for (uint32_t v=0;v<flow->values;++v) {
            if (!xir_compile_work(flow->work,2)) return XR_XIR_BUDGET;
            if (!flow->terms[v]) continue;
            XrXirRootTerm term=effect_formula_term(flow,v);
            if (term.kind!=kind) continue;
            if (!different && (to->terms[at].kind!=term.kind || to->terms[at].index!=term.index)) different=true;
            ++at;
        }
    }
    if (!different) return XR_XIR_OK;
    XrXirStatus status=XR_XIR_OK;
    XrXirRootTerm *terms=count ? xir_compile_calloc(flow->work,count,sizeof(*terms),&status) : NULL;
    if (count && !terms) return status;
    at=0;
    for (uint32_t kind=XR_XIR_ROOT_TERM_PARAMETER;kind<=XR_XIR_ROOT_TERM_CELL_PARAMETER && status==XR_XIR_OK;++kind) {
        for (uint32_t v=0;v<flow->values;++v) {
            if (!xir_compile_work(flow->work,2)) { status=XR_XIR_BUDGET;break; }
            if (!flow->terms[v]) continue;
            XrXirRootTerm term=effect_formula_term(flow,v);
            if (term.kind==kind) terms[at++]=term;
        }
    }
    if (status!=XR_XIR_OK) { xr_compile_resources_free(terms);return status; }
    if (!xir_compile_work(flow->work,3)) { xr_compile_resources_free(terms);return XR_XIR_BUDGET; }
    xr_compile_resources_free((void *)to->terms);
    *to=(XrXirRootFormula){constant,count,terms};*changed=true;return XR_XIR_OK;
}

/* A complete physical-value scan proves that no callable origin can be
 * seeded. All executable and latent call families retain their full flow. */
static XrXirStatus effect_formula_needs_value_flow(EffectParameterFlow *flow,bool *needed) {
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
/* The proven zero origin matrix needs only its actual equation terms. Cell
 * dependencies continue to come from the complete shared provenance proof. */
static XrXirStatus effect_formula_zero_origins(EffectParameterFlow *flow,EffectGraph *graph) {
    uint64_t values=(uint64_t)flow->function->parameter_count+flow->function->instruction_count;
    if (values>UINT32_MAX || values>SIZE_MAX) return XR_XIR_BUDGET;
    flow->values=(uint32_t)values;
    size_t bytes=(size_t)values;
    if (bytes>graph->flow_capacity) {
        xr_compile_resources_free(graph->flow_rows);
        graph->flow_rows=NULL;graph->flow_capacity=0;
        XrXirStatus status=XR_XIR_OK;
        graph->flow_rows=xir_compile_calloc(flow->work,1,bytes,&status);
        if (bytes && !graph->flow_rows) return status;
        graph->flow_capacity=bytes;
    } else if (bytes) {
        if (!xir_compile_work(flow->work,bytes)) return XR_XIR_BUDGET;
        memset(graph->flow_rows,0,bytes);
    }
    flow->terms=(uint8_t *)graph->flow_rows;return XR_XIR_OK;
}

static XrXirStatus effect_formula_function(const XrXirModule *module,
    XrXirEffects *effects, uint32_t function, EffectGraph *graph,
    const XrXirCompileContext *work, bool *changed) {
    EffectParameterFlow flow = {0};
    flow.work = work; flow.module = module; flow.effects = effects;
    flow.function = &module->functions[function]; flow.formula_phase = true;
    flow.parameters = (XrXirEffectParameter *)effects->contracts[function].parameters;
    bool value_flow=true;
    XrXirStatus status=effect_formula_needs_value_flow(&flow,&value_flow);
    if (status==XR_XIR_OK) status=value_flow ? effect_parameter_rows(&flow,graph) :
        effect_formula_zero_origins(&flow,graph);
    if (status==XR_XIR_OK) status=effect_formula_witness_start(&flow);
    flow.changed = value_flow;
    while (status == XR_XIR_OK && flow.changed) {
        flow.changed = false;
        for (uint32_t i = 0; status == XR_XIR_OK && i < flow.function->instruction_count; ++i)
            status = effect_parameter_instruction(&flow,&flow.function->instructions[i]);
    }
    /* Set advertised latent masks only after origins have converged. A late
     * PHI/capture producer must not prematurely poison its symbolic equation. */
    flow.capture_masks = true; flow.changed = value_flow;
    while (status == XR_XIR_OK && flow.changed) {
        flow.changed = false;
        for (uint32_t i = 0; status == XR_XIR_OK && i < flow.function->instruction_count; ++i)
            status = effect_parameter_instruction(&flow,&flow.function->instructions[i]);
    }
    uint32_t constant = 0;
    const XrXirDeclarations *declarations = module->declarations;
    if (status == XR_XIR_OK && declarations) {
        if (!xir_compile_work(work, 1)) status = XR_XIR_BUDGET;
        else if (declarations->modules[declarations->functions[function].module].initializer == function) {
            constant = XR_XIR_CALLABLE_ROOT_REQUIRED;
            XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_INITIALIZER,UINT32_MAX,UINT32_MAX,UINT32_MAX,0};
            status=effect_formula_witness_add(&flow,constant,witness,true);
        }
    }
    for (uint32_t i = 0; status == XR_XIR_OK && i < flow.function->instruction_count; ++i)
        status = effect_formula_instruction(&flow,i,&constant);
    if (status == XR_XIR_OK) status = effect_formula_publish(&flow,function,constant,changed);
    return status;
}

static XrXirStatus effect_formulas_derive(const XrXirModule *module,
    XrXirEffects *effects, EffectGraph *graph, const XrXirCompileContext *work) {
    if (!effects->contracts) return XR_XIR_OK;
    uint32_t front = 0, back = 0, pending = effects->count;
    for (uint32_t f = 0; f < effects->count; ++f) {
        if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
        graph->queue[f] = f; graph->queued[f] = 1;
    }
    while (pending) {
        if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
        uint32_t callee = graph->queue[front];
        front = front + 1 == effects->count ? 0 : front + 1;
        --pending; graph->queued[callee] = 0;
        bool changed = false;
        XrXirStatus status = effect_formula_function(module,effects,callee,graph,work,&changed);
        if (status != XR_XIR_OK) return status;
        if (!changed) continue;
        for (uint32_t e = graph->heads[callee]; e != UINT32_MAX; e = graph->edges[e].next) {
            if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
            uint32_t caller = graph->edges[e].caller;
            if (graph->queued[caller]) continue;
            if (!xir_compile_work(work, 3)) return XR_XIR_BUDGET;
            graph->queue[back] = caller; graph->queued[caller] = 1;
            back = back + 1 == effects->count ? 0 : back + 1; ++pending;
        }
        /* This owner rebuilt latent references from real instructions. They
         * wake formula users without becoming executing/root/GO edges. */
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
        for (uint32_t e = graph->reference_heads ? graph->reference_heads[callee] : UINT32_MAX;
             e != UINT32_MAX; e = graph->references[e].next) {
            if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
            uint32_t caller = graph->references[e].caller;
            if (graph->queued[caller]) continue;
            if (!xir_compile_work(work, 3)) return XR_XIR_BUDGET;
            graph->queue[back] = caller; graph->queued[caller] = 1;
            back = back + 1 == effects->count ? 0 : back + 1; ++pending;
        }
    }
    return XR_XIR_OK;
}

/* Projection is for the declaration's advertised environment only. Direct
 * callers substitute their true actuals in the formula solver above. */
static XrXirStatus effect_formulas_project(const XrXirModule *module,
    XrXirEffects *effects, const XrXirCompileContext *work) {
    if (!effects->contracts) return XR_XIR_OK;
    for (uint32_t f=0;f<effects->count;++f) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        const XrXirRootFormula *formula=&effects->contracts[f].formula;
        uint32_t mask=formula->constant_mask;
        for (uint32_t t=0;t<formula->term_count;++t) {
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            const XrXirRootTerm *term=&formula->terms[t];
            if (term->kind==XR_XIR_ROOT_TERM_CONTEXT_CALL) mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
            else if (term->kind==XR_XIR_ROOT_TERM_CELL_PARAMETER) {
                if (term->index>=module->functions[f].parameter_count ||
                    !xr_xir_type_is_cell(module->types,module->functions[f].parameters[term->index]))
                    return XR_XIR_BAD_STRUCTURE;
                mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
            }
            else if (term->kind==XR_XIR_ROOT_TERM_PARAMETER) {
                if (term->index>=module->functions[f].parameter_count ||
                    effects->contracts[f].parameters[term->index].kind!=XR_XIR_EFFECT_PARAMETER_VARIABLE)
                    return XR_XIR_BAD_STRUCTURE;
                const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,
                    module->functions[f].parameters[term->index]);
                if (!signature) return XR_XIR_BAD_TYPE;
                mask|=signature->flags&(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED);
            } else return XR_XIR_BAD_STRUCTURE;
        }
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        effects->root[f]=(XrXirRootEffects){!!(mask&XR_XIR_CALLABLE_ROOT_REQUIRED),
            !!(mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)};
    }
    return XR_XIR_OK;
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
