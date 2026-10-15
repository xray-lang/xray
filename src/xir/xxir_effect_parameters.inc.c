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
    EffectBodyRequest body;
    EffectBodyFunctionHead head;
    uint32_t function_index, instruction;
    XrXirEffects *effects;
    const EffectBodyFunctionHead *function;
    XrXirEffectParameter *parameters;
    uint64_t *rows;
    uint32_t *previous_uses, *masks;
    uint32_t *first_reads, *first_callers;
    uint8_t *conditional, *terms;
    uint32_t words, values;
    bool changed, feedback, formula_phase, capture_masks;
} EffectParameterFlow;

static const uint8_t effect_parameter_operands[XR_XIR_OP_COUNT] = {
    0,
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) operands,
#include "xxir_ops.def"
#undef XR_XIR_OP
};

static void effect_parameters_free(XrXirEffects *effects) {
    /* The forest may already own or have released the unique Result.
     * Retire its views without dereferencing the borrowed storage owner. */
    bool owned_result=effects->result_borrow!=NULL;
    effect_invocation_certificate_free(effects->invocations);effects->invocations=NULL;
    effects->cells=NULL;
    effect_body_material_free(effects->generation);effects->generation=NULL;
    if (!owned_result) xr_compile_resources_free(effects->formula_terminals);
    effects->formula_terminals=NULL;
    if (!owned_result) xr_compile_resources_free(effects->constant_witnesses);
    effects->constant_witnesses=NULL;effects->result_borrow=NULL;
    if (!effects->contracts) return;
    for (uint32_t f = 0; f < effects->count; ++f) {
        xr_compile_resources_free((void *)effects->contracts[f].parameters);
        if (!owned_result) xr_compile_resources_free((void *)effects->contracts[f].formula.terms);
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
static inline XrXirStatus effect_parameters_variable_domain(const XrXirModule *module,
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
    XrXirEffects *effects, EffectGraph *graph,const XrXirCompileContext *work) {
    if (!graph || !module || effects->count!=module->function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    if (!graph->material || !graph->physical_owned)
        status=effect_graph_physical_refresh(work,module,graph,effects->refinement);
    if (status!=XR_XIR_OK) return status;
    EffectBodyMaterial *material=graph->material;
    if (!effect_body_material_shape(material) || material->resources!=work->resources ||
        effect_body_count(material)!=effects->count || !material->cells)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
    effect_parameters_free(effects);
    effects->generation=material;effects->cells=material->cells;graph->physical_owned=false;
    effects->contracts = xir_compile_calloc(work, effects->count,
        sizeof(*effects->contracts), &status);
    if (!effects->contracts) return status;
    for (uint32_t f=0;f<effects->count;++f) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        const EffectBodyFunction *function=&material->functions[f];
        uint32_t count=function->parameter_count;
        status=effect_body_initial_roles_match(work,material,f);
        if (status!=XR_XIR_OK) return status;
        XrXirFunctionEffectContract *contract=&effects->contracts[f];contract->parameter_count=count;
        if (count) {
            contract->parameters=xir_compile_calloc(work,count,sizeof(*contract->parameters),&status);
            if (!contract->parameters) return status;
        }
        XrXirEffectParameter *parameters=(XrXirEffectParameter *)contract->parameters;
        for (uint32_t v=0;v<count;++v) {
            if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
            uint32_t kind=function->initial_roles[v];
            parameters[v].kind=kind;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_parameter_type(EffectParameterFlow *flow,uint32_t value,XrXirType *type) {
    return effect_body_request_type(&flow->body,flow->function_index,value,type);
}

static XrXirStatus effect_parameter_operation(EffectParameterFlow *flow,uint32_t instruction,
    XrXirInstruction *storage,const XrXirInstruction **output) {
    return effect_body_request_instruction(&flow->body,flow->function_index,instruction,storage,output);
}

/* Only a write after a consumer has run can require another ascending pass.
 * Keep the earliest actual reader, including PHI backedges and local writes. */
static XrXirStatus effect_parameter_read(EffectParameterFlow *flow, uint32_t value) {
    if (!flow->first_reads) return XR_XIR_OK;
    if (!xir_compile_work(flow->work, 2)) return XR_XIR_BUDGET;
    if (flow->instruction < flow->first_reads[value])
        flow->first_reads[value] = flow->instruction;
    return XR_XIR_OK;
}

static XrXirStatus effect_parameter_join(EffectParameterFlow *flow,
    uint32_t destination, uint32_t source) {
    if (destination >= flow->values || source >= flow->values) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = effect_parameter_read(flow, source);
    if (status != XR_XIR_OK) return status;
    uint64_t *to = flow->rows + (size_t)destination * flow->words;
    const uint64_t *from = flow->rows + (size_t)source * flow->words;
    for (uint32_t w = 0; w < flow->words; ++w) {
        if (!xir_compile_work(flow->work, 3)) return XR_XIR_BUDGET;
        uint64_t joined = to[w] | from[w];
        if (joined != to[w]) {
            flow->changed = true;
            if (flow->first_reads) {
                if (!xir_compile_work(flow->work, 1)) return XR_XIR_BUDGET;
                if (flow->first_reads[destination] <= flow->instruction) flow->feedback = true;
            }
        }
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
    XrXirStatus status = effect_parameter_read(flow, source);
    if (status != XR_XIR_OK) return status;
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
        flow->instruction;
    if (op->args[1] > callee->parameter_count) return XR_XIR_BAD_STRUCTURE;
    if (op->args[1] && flow->first_callers) {
        if (!xir_compile_work(flow->work, 2)) return XR_XIR_BUDGET;
        if (flow->function_index < flow->first_callers[target])
            flow->first_callers[target] = flow->function_index;
    }
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
        flow->instruction;
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
        count = xr_xir_slot_payload_operands(flow->body.domain.declarations->slots,
            flow->body.domain.declarations->slot_count, op);
    if (op->op == XR_XIR_CELL_NEW || op->op == XR_XIR_CELL_READ ||
        op->op == XR_XIR_CELL_WRITE || op->op == XR_XIR_CELL_LOCAL_WRITE) {
        XrXirType owner_type=op->type;
        if (op->op!=XR_XIR_CELL_NEW) {
            XrXirStatus status=effect_parameter_type(flow,op->args[0],&owner_type);
            if (status!=XR_XIR_OK) return status;
        }
        count=xr_xir_cell_payload_operands(flow->body.domain.types,owner_type,op->op);
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
        if (xr_xir_cell_is_unit(flow->body.domain.types,op->type)) return XR_XIR_OK;
        return effect_parameter_transfer(flow, destination, op->args[0], XR_XIR_EFFECT_USE_STORE);
    case XR_XIR_NULLABLE_SOME:
        return effect_parameter_transfer(flow, destination, op->args[0], XR_XIR_EFFECT_USE_STORE);
    case XR_XIR_CELL_READ: case XR_XIR_NULLABLE_UNWRAP: case XR_XIR_PLACE_READ:
    case XR_XIR_STRUCT_GET: case XR_XIR_CLASS_GET: case XR_XIR_ENUM_GET: case XR_XIR_TUPLE_FIELD:
        return effect_parameter_transfer(flow, destination, op->args[0], XR_XIR_EFFECT_USE_COPY);
    case XR_XIR_CELL_WRITE: case XR_XIR_CELL_LOCAL_WRITE: {
        XrXirType type;XrXirStatus status=effect_parameter_type(flow,op->args[0],&type);
        if (status!=XR_XIR_OK) return status;
        if (xr_xir_cell_is_unit(flow->body.domain.types,type)) return XR_XIR_OK;
        return effect_parameter_transfer(flow, op->args[0], op->args[1], XR_XIR_EFFECT_USE_STORE);
    }
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
    else tail += values * sizeof(uint32_t);
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
    flow->first_reads = flow->formula_phase ? NULL : tail_words + flow->function->parameter_count;
    flow->masks = flow->formula_phase ? tail_words + flow->function->parameter_count : NULL;
    flow->conditional = (uint8_t *)(tail_words + flow->function->parameter_count +
        (size_t)values);
    flow->terms = flow->formula_phase ? flow->conditional + flow->function->parameter_count : NULL;
    if (flow->first_reads) {
        if (!xir_compile_work(flow->work, values * sizeof(uint32_t))) return XR_XIR_BUDGET;
        memset(flow->first_reads, 0xff, (size_t)values * sizeof(uint32_t));
    }
    for (uint32_t p = 0; p < flow->function->parameter_count; ++p) {
        if (!xir_compile_work(flow->work, 3)) return XR_XIR_BUDGET;
        const XrXirTypeNode *signature = xr_xir_callable_signature(flow->body.domain.types,flow->function->parameters[p]);
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
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        XrXirStatus status=effect_parameter_operation(flow,i,&storage,&op);
        if (status!=XR_XIR_OK) return status;
        const XrXirTypeNode *signature = xr_xir_callable_signature(flow->body.domain.types,op->type);
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

static XrXirStatus effect_parameter_solve(EffectParameterFlow *flow) {
    XrXirStatus status = XR_XIR_OK;
    flow->feedback = true;
    while (status == XR_XIR_OK && flow->feedback) {
        flow->feedback = false;
        for (uint32_t i=0;status==XR_XIR_OK && i<flow->function->instruction_count;++i) {
            XrXirInstruction storage;const XrXirInstruction *op=NULL;
            flow->instruction=i;status=effect_parameter_operation(flow,i,&storage,&op);
            if (status==XR_XIR_OK) status=effect_parameter_instruction(flow,op);
        }
    }
    return status;
}

static XrXirStatus effect_parameter_function(const XrXirModule *module,
    XrXirEffects *effects, uint32_t function, EffectGraph *graph,
    const XrXirCompileContext *work, bool *changed) {
    EffectParameterFlow flow = {0};
    (void)module;
    if (!effects->generation || graph->material!=effects->generation) return XR_XIR_BAD_STRUCTURE;
    flow.work=work;flow.effects=effects;flow.function_index=function;
    flow.first_callers = graph->parameter_first_callers;
    flow.body=effect_body_owned_request(work,effects->generation);flow.function=&flow.head;
    XrXirStatus status=effect_body_request_head(&flow.body,function,&flow.head);
    if (status!=XR_XIR_OK) return status;
    flow.parameters = (XrXirEffectParameter *)effects->contracts[function].parameters;
    bool has_callable = false;
    for (uint32_t p = 0; p < flow.function->parameter_count; ++p) {
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
        if (xr_xir_callable_signature(flow.body.domain.types, flow.function->parameters[p])) has_callable = true;
    }
    if (!has_callable) return XR_XIR_OK;
    status = effect_parameter_rows(&flow, graph);
    if (status == XR_XIR_OK) status = effect_parameter_solve(&flow);
    for (uint32_t p = 0; status == XR_XIR_OK && p < flow.function->parameter_count; ++p) {
        if (!xir_compile_work(work, 3)) { status = XR_XIR_BUDGET; break; }
        XrXirEffectParameter *parameter = &flow.parameters[p];
        if (!xr_xir_callable_signature(flow.body.domain.types, flow.function->parameters[p])) continue;
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
    XrXirStatus status = effect_parameters_allocate(module,effects,graph,work);
    if (status != XR_XIR_OK || !effects->contracts) return status;
    if (!effect_body_variable_roles(effects->generation)) return XR_XIR_OK;
    if (effects->count > graph->parameter_capacity) {
        xr_compile_resources_free(graph->parameter_first_callers);
        graph->parameter_first_callers = NULL; graph->parameter_capacity = 0;
        graph->parameter_first_callers = xir_compile_calloc(work, effects->count,
            sizeof(*graph->parameter_first_callers), &status);
        if (!graph->parameter_first_callers) return status;
        graph->parameter_capacity = effects->count;
    }
    if (!xir_compile_work(work, (uint64_t)effects->count * sizeof(uint32_t))) return XR_XIR_BUDGET;
    if (effects->count) memset(graph->parameter_first_callers, 0xff,
        (size_t)effects->count * sizeof(uint32_t));
    /* Later callers observe a new summary in this pass. Earlier callers and
     * recursive calls rewind the next pass to the earliest actual consumer. */
    uint32_t first = 0;
    while (status == XR_XIR_OK && first < effects->count) {
        uint32_t next = effects->count;
        for (uint32_t f = first; status == XR_XIR_OK && f < effects->count; ++f) {
            bool changed = false;
            status = effect_parameter_function(module, effects, f, graph, work, &changed);
            if (status != XR_XIR_OK) break;
            if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
            uint32_t caller = graph->parameter_first_callers[f];
            if (changed && caller <= f && caller < next) next = caller;
        }
        first = next;
    }
    return status;
}

/* A charged structural probe retained for the original negative controls.
 * It does not derive a formula or authorize execution. */
static inline XrXirStatus effect_formula_needs_value_flow(EffectParameterFlow *flow,bool *needed) {
    *needed=true;
    for (uint32_t p=0;p<flow->function->parameter_count;++p) {
        if (!xir_compile_work(flow->work,1)) return XR_XIR_BUDGET;
        if (xr_xir_callable_signature(flow->body.domain.types,flow->function->parameters[p]))
            return XR_XIR_OK;
    }
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(flow->work,1)) return XR_XIR_BUDGET;
        EffectBodyInstructionShape shape={0};
        XrXirStatus status=effect_body_request_shape(&flow->body,flow->function_index,i,&shape);
        if (status!=XR_XIR_OK) return status;
        if (xr_xir_callable_signature(flow->body.domain.types,shape.type)) return XR_XIR_OK;
        switch (shape.op) {
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
