/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_scalar.inc.c - Complete callable bounds on scalar carriers
 *
 * KEY CONCEPT:
 *   Carrier transfer keeps every nested type, mode, and result exact. Explicit
 *   weakening remains a real barrier; latent capture reads a derived formula.
 */
typedef struct EffectScalarFlow {
    EffectTerms *terms;
    XrXirFunction *function;
    const EffectOrdinaryContexts *contexts;
    const XrXirType *declared;
    XrXirType *values;
    uint8_t *fixed;
    uint32_t value_count;
    bool changed;
    const XrXirDeclarations *declarations;
} EffectScalarFlow;

static uint32_t effect_scalar_flags_join(uint32_t left, uint32_t right) {
    uint32_t root=(left|right)&(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    return (left&right&XR_XIR_CALLABLE_NO_SUSPEND) | (root ? root : XR_XIR_CALLABLE_ROOT_NONE);
}

static XrXirStatus effect_scalar_bound(EffectTerms *terms,
    XrXirType left, XrXirType right, XrXirType *output) {
    if (!xir_compile_work(terms->remaining,1)) return XR_XIR_BUDGET;
    const XrXirTypeNode *first=xr_xir_callable_signature(&terms->types,left);
    const XrXirTypeNode *second=xr_xir_callable_signature(&terms->types,right);
    if (!first || !second) return XR_XIR_BAD_TYPE;
    XrXirTypeNode a=*first,b=*second;
    if (a.parameter_count!=b.parameter_count) return XR_XIR_BAD_TYPE;
    XrXirTypeMatchScratch scratch={terms->remaining->resources,NULL};
    XrXirStatus status=xr_xir_compile_type_substitution_matches_between_scratch(terms->remaining,
        &terms->types,&terms->types,NULL,0,a.result,b.result,&scratch);
    for (uint32_t p=0;p<a.parameter_count && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(terms->remaining,1)) { status=XR_XIR_BUDGET;break; }
        if (a.parameters[p].mode!=b.parameters[p].mode) { status=XR_XIR_BAD_TYPE;break; }
        status=xr_xir_compile_type_substitution_matches_between_scratch(terms->remaining,
            &terms->types,&terms->types,NULL,0,a.parameters[p].type,b.parameters[p].type,&scratch);
    }
    xr_xir_type_match_scratch_free(&scratch);
    if (status!=XR_XIR_OK) return status;
    a.flags=effect_scalar_flags_join(a.flags,b.flags);
    *output=effect_term_intern(terms,a);return terms->status;
}

static XrXirStatus effect_scalar_assign(EffectScalarFlow *flow, uint32_t destination, XrXirType actual) {
    if (destination>=flow->value_count) return XR_XIR_BAD_VALUE;
    if (!xir_compile_work(flow->terms->remaining,1)) return XR_XIR_BUDGET;
    const XrXirTypeNode *declared=xr_xir_callable_signature(&flow->terms->types,flow->declared[destination]);
    if (!declared) return XR_XIR_OK;
    if (actual==XR_XIR_UNIT) return XR_XIR_OK;
    if (flow->values[destination]!=XR_XIR_UNIT) {
        XrXirStatus status=effect_scalar_bound(flow->terms,flow->values[destination],actual,&actual);
        if (status!=XR_XIR_OK) return status;
    }
    XirEffectCallableBound request={&flow->terms->types,&flow->terms->types,NULL,0,
        flow->declared[destination],actual};
    XrXirStatus status=xir_effect_callable_bound_matches(flow->terms->remaining,&request);
    if (status!=XR_XIR_OK) return status;
    if (flow->values[destination]!=actual) flow->changed=true;
    flow->values[destination]=actual;
    if (destination>=flow->function->parameter_count)
        ((XrXirInstruction *)flow->function->instructions)[destination-flow->function->parameter_count].type=actual;
    return XR_XIR_OK;
}

static XrXirStatus effect_scalar_transfer(EffectScalarFlow *flow, uint32_t destination, uint32_t source) {
    if (source>=flow->value_count) return XR_XIR_BAD_VALUE;
    if (destination>=flow->value_count) return XR_XIR_BAD_VALUE;
    if (!xir_compile_work(flow->terms->remaining,2)) return XR_XIR_BUDGET;
    if (flow->fixed[source] && !flow->fixed[destination]) {
        flow->fixed[destination]=1;flow->changed=true;
    }
    return effect_scalar_assign(flow,destination,flow->values[source]);
}

static XrXirStatus effect_scalar_phi(EffectScalarFlow *flow, uint32_t instruction) {
    XrXirInstruction op=flow->function->instructions[instruction];
    uint32_t destination=flow->function->parameter_count+instruction;
    if (!xr_xir_callable_signature(&flow->terms->types,flow->declared[destination])) return XR_XIR_OK;
    if (!op.args[1] || op.args[1]%2 || !flow->function->operands ||
        op.args[0]>flow->function->operand_count ||
        op.args[1]>flow->function->operand_count-op.args[0]) return XR_XIR_BAD_STRUCTURE;
    XrXirType joined=XR_XIR_UNIT;
    for (uint32_t a=1;a<op.args[1];a+=2) {
        if (!xir_compile_work(flow->terms->remaining,1)) return XR_XIR_BUDGET;
        uint32_t value=flow->function->operands[op.args[0]+a];
        if (value>=flow->value_count) return XR_XIR_BAD_VALUE;
        if (!xir_compile_work(flow->terms->remaining,2)) return XR_XIR_BUDGET;
        if (flow->fixed[value] && !flow->fixed[destination]) {
            flow->fixed[destination]=1;flow->changed=true;
        }
        if (flow->values[value]==XR_XIR_UNIT) continue;
        if (joined==XR_XIR_UNIT) joined=flow->values[value];
        else {
            XrXirStatus status=effect_scalar_bound(flow->terms,joined,flow->values[value],&joined);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return effect_scalar_assign(flow,destination,joined);
}

static XrXirStatus effect_scalar_capture(EffectScalarFlow *flow, uint32_t instruction) {
    XrXirInstruction op=flow->function->instructions[instruction];
    uint32_t destination=flow->function->parameter_count+instruction;
    const XrXirTypeNode *signature=xr_xir_callable_signature(&flow->terms->types,flow->declared[destination]);
    if (!signature) return XR_XIR_BAD_TYPE;
    XrXirTypeNode bound=*signature;
    const EffectOrdinaryContexts *contexts=flow->contexts;
    if (!contexts || contexts->uses.resources!=flow->terms->remaining->resources ||
        !contexts->uses.root || !contexts->uses.constant_witnesses || !contexts->uses.formula_terminals ||
        contexts->terms.types.nodes!=flow->terms->types.nodes || op.immediate<0 ||
        (uint64_t)op.immediate>=contexts->count || !contexts->uses.contracts)
        return XR_XIR_BAD_STRUCTURE;
    if (op.args[1] && (!flow->function->operands || op.args[0]>flow->function->operand_count ||
        op.args[1]>flow->function->operand_count-op.args[0])) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionEffectContract *callee=&contexts->uses.contracts[op.immediate];
    if (op.args[1]>callee->parameter_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t mask=callee->formula.constant_mask;
    /* The dense owner has already authenticated this real capture edge and
     * retained declared bounds for fixed or missing suffix actuals. Consume
     * its complete equation, including conditional witness/default calls. */
    if (contexts->dense) {
        XrXirStatus status=effect_formula_edge_mask(flow->terms->remaining,&flow->terms->types,
            &contexts->uses,contexts->functions,(uint32_t)(flow->function-contexts->functions),
            &op,(uint32_t)op.immediate,&mask);
        if (status!=XR_XIR_OK) return status;
        bound.flags=(bound.flags&XR_XIR_CALLABLE_NO_SUSPEND)|(mask?mask:XR_XIR_CALLABLE_ROOT_NONE);
        XrXirType actual=effect_term_intern(flow->terms,bound);
        if (flow->terms->status!=XR_XIR_OK) return flow->terms->status;
        return effect_scalar_assign(flow,destination,actual);
    }
    for (uint32_t t=0;t<callee->formula.term_count;++t) {
        if (!xir_compile_work(flow->terms->remaining,3)) return XR_XIR_BUDGET;
        XrXirRootTerm term=callee->formula.terms[t];
        if (term.kind!=XR_XIR_ROOT_TERM_PARAMETER || term.index>=op.args[1] ||
            callee->parameters[term.index].kind!=XR_XIR_EFFECT_PARAMETER_VARIABLE)
            return effect_scalar_assign(flow,destination,flow->declared[destination]);
        uint32_t value=flow->function->operands[op.args[0]+term.index];
        if (value>=flow->value_count) return XR_XIR_BAD_VALUE;
        if (flow->values[value]==XR_XIR_UNIT) return XR_XIR_OK;
        XrXirType selected=flow->fixed[value] ?
            contexts->functions[op.immediate].parameters[term.index] : flow->values[value];
        const XrXirTypeNode *actual=xr_xir_callable_signature(&flow->terms->types,selected);
        if (!actual) return XR_XIR_BAD_TYPE;
        mask|=actual->flags&(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    }
    bound.flags=(bound.flags&XR_XIR_CALLABLE_NO_SUSPEND)|(mask ? mask : XR_XIR_CALLABLE_ROOT_NONE);
    XrXirType actual=effect_term_intern(flow->terms,bound);
    if (flow->terms->status!=XR_XIR_OK) return flow->terms->status;
    return effect_scalar_assign(flow,destination,actual);
}

static XrXirStatus effect_scalar_escape(EffectScalarFlow *flow, uint32_t instruction) {
    XrXirInstruction op=flow->function->instructions[instruction];
    uint32_t destination=flow->function->parameter_count+instruction;
    bool construct=op.op==XR_XIR_CELL_NEW || op.op==XR_XIR_NULLABLE_SOME ||
        op.op==XR_XIR_ARRAY_NEW || op.op==XR_XIR_STRUCT_NEW || op.op==XR_XIR_CLASS_NEW ||
        op.op==XR_XIR_ENUM_NEW || op.op==XR_XIR_TUPLE_NEW;
    bool store=construct || op.op==XR_XIR_CELL_WRITE || op.op==XR_XIR_CELL_LOCAL_WRITE ||
        op.op==XR_XIR_PLACE_WRITE || op.op==XR_XIR_SLOT_STORE || op.op==XR_XIR_SLOT_INIT ||
        op.op==XR_XIR_SLOT_GROUP_INIT || op.op==XR_XIR_ARRAY_SET || op.op==XR_XIR_ARRAY_PUSH ||
        op.op==XR_XIR_STRUCT_SET || op.op==XR_XIR_CLASS_SET;
    if (!store) return XR_XIR_OK;
    if (construct && !flow->fixed[destination]) { flow->fixed[destination]=1;flow->changed=true; }
    bool table=xr_xir_op_uses_operand_table(op.op);
    uint32_t count=table ? op.args[1] : effect_parameter_operands[op.op];
    if (op.op==XR_XIR_SLOT_STORE || op.op==XR_XIR_SLOT_INIT) {
        if (!flow->declarations || op.immediate<0 ||
            (uint64_t)op.immediate>=flow->declarations->slot_count) return XR_XIR_BAD_STRUCTURE;
        count=xr_xir_slot_payload_operands(flow->declarations->slots,flow->declarations->slot_count,&op);
    }
    if (op.op==XR_XIR_CELL_NEW || op.op==XR_XIR_CELL_WRITE || op.op==XR_XIR_CELL_LOCAL_WRITE) {
        XrXirType owner_type=op.op==XR_XIR_CELL_NEW ? op.type : xr_xir_operand_type(flow->function,op.args[0]);
        count=xr_xir_cell_payload_operands(&flow->terms->types,owner_type,op.op);
    }
    if (table && ((count && !flow->function->operands) || op.args[0]>flow->function->operand_count ||
        count>flow->function->operand_count-op.args[0])) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t a=0;a<count;++a) {
        if (!xir_compile_work(flow->terms->remaining,2)) return XR_XIR_BUDGET;
        uint32_t value=table ? flow->function->operands[op.args[0]+a] : op.args[a];
        if (value>=flow->value_count) return XR_XIR_BAD_VALUE;
        if (!flow->fixed[value]) { flow->fixed[value]=1;flow->changed=true; }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_scalar_instruction(EffectScalarFlow *flow, uint32_t instruction) {
    if (!xir_compile_work(flow->terms->remaining,1)) return XR_XIR_BUDGET;
    XrXirInstruction op=flow->function->instructions[instruction];
    uint32_t destination=flow->function->parameter_count+instruction;
    XrXirStatus escape=effect_scalar_escape(flow,instruction);
    if (escape!=XR_XIR_OK) return escape;
    switch (op.op) {
    case XR_XIR_COPY: case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ:
        return effect_scalar_transfer(flow,destination,op.args[0]);
    case XR_XIR_CELL_READ: case XR_XIR_NULLABLE_UNWRAP: case XR_XIR_STRUCT_GET:
    case XR_XIR_CLASS_GET: case XR_XIR_ENUM_GET: case XR_XIR_TUPLE_FIELD:
    case XR_XIR_ARRAY_GET: case XR_XIR_PLACE_READ:
        if (op.args[0]>=flow->value_count) return XR_XIR_BAD_VALUE;
        if (flow->fixed[op.args[0]] && !flow->fixed[destination]) {
            flow->fixed[destination]=1;flow->changed=true;
        }
        return XR_XIR_OK;
    case XR_XIR_LOCAL_WRITE:
        return effect_scalar_transfer(flow,op.args[0],op.args[1]);
    case XR_XIR_PHI:
        return effect_scalar_phi(flow,instruction);
    case XR_XIR_FUNCTION_REF:
        return effect_scalar_capture(flow,instruction);
    case XR_XIR_FUNCTION_WEAKEN:
        if (!xir_compile_work(flow->terms->remaining,1)) return XR_XIR_BUDGET;
        if (!flow->fixed[destination]) { flow->fixed[destination]=1;flow->changed=true; }
        return XR_XIR_OK;
    default:
        return XR_XIR_OK;
    }
}

/* The caller must supply freshly derived contextual contracts and already
 * selected physical actuals. This helper cannot select a dense vector itself. */
static XrXirStatus effect_scalar_flow(EffectScalarFlow *flow) {
    if (!flow || !flow->terms || !flow->terms->remaining || !flow->function ||
        !flow->declared || !flow->values || !flow->fixed ||
        (uint64_t)flow->function->parameter_count+flow->function->instruction_count!=flow->value_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(flow->terms->remaining,1)) return XR_XIR_BUDGET;
        XrXirOp op=flow->function->instructions[i].op;
        flow->fixed[flow->function->parameter_count+i]=0;
        if (op==XR_XIR_COPY || op==XR_XIR_LOCAL_NEW || op==XR_XIR_LOCAL_READ ||
            op==XR_XIR_PHI || op==XR_XIR_FUNCTION_REF)
            flow->values[flow->function->parameter_count+i]=XR_XIR_UNIT;
    }
    flow->changed=true;
    while (status==XR_XIR_OK && flow->changed) {
        flow->changed=false;
        for (uint32_t i=0;i<flow->function->instruction_count && status==XR_XIR_OK;++i)
            status=effect_scalar_instruction(flow,i);
    }
    for (uint32_t v=0;v<flow->value_count && status==XR_XIR_OK;++v) {
        if (!xir_compile_work(flow->terms->remaining,1)) return XR_XIR_BUDGET;
        if (flow->values[v]==XR_XIR_UNIT && xr_xir_callable_signature(&flow->terms->types,flow->declared[v]))
            status=XR_XIR_BAD_TYPE;
    }
    return status;
}
