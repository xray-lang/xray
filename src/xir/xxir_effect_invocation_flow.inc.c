/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_flow.inc.c - Finite real callable and common Cell origin projection
 */
typedef struct EffectInvocationFlow {
    EffectInvocationOwner *owner;
    const XrXirFunction *function;
    EffectInvocationBasis basis;
    uint32_t node,values,instruction;
    uint64_t *rows;
    uint8_t *fixed,*certified;
    void *memory;
    bool changed,precision;
} EffectInvocationFlow;

static XrXirStatus effect_invocation_cell(EffectInvocationFlow *flow,
    uint32_t value,uint64_t *output,bool *changed);

/* Outer callable bounds use the shared complete result/parameter/mode
 * matcher. Every storage type and every structural child remains exact. */
static XrXirStatus effect_invocation_value_matches(EffectInvocationOwner *owner,
    XrXirType expected,XrXirType actual,XrXirTypeMatchScratch *scratch) {
    if (xr_xir_callable_signature(owner->module->types,expected)) {
        XirEffectCallableBound request={owner->module->types,owner->module->types,
            NULL,0,expected,actual};
        return effect_callable_bound_matches_scratch(owner->work,&request,scratch);
    }
    return xr_xir_compile_type_substitution_matches_between_scratch(owner->work,
        owner->module->types,owner->module->types,NULL,0,expected,actual,scratch);
}

static XrXirStatus effect_invocation_origin_bit(EffectInvocationFlow *flow,
    uint32_t value,uint32_t bit) {
    if (value>=flow->values || bit/64>=flow->basis.origin_words) return XR_XIR_BAD_VALUE;
    if (!xir_compile_work(flow->owner->work,3)) return XR_XIR_BUDGET;
    uint64_t *word=flow->rows+(size_t)value*flow->basis.fn_words+bit/64;
    uint64_t joined=*word|(UINT64_C(1)<<(bit%64));
    if (joined!=*word) flow->changed=true;
    *word=joined;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_origin_join(EffectInvocationFlow *flow,
    uint32_t destination,uint32_t source) {
    if (destination>=flow->values || source>=flow->values) return XR_XIR_BAD_VALUE;
    uint64_t *to=flow->rows+(size_t)destination*flow->basis.fn_words;
    const uint64_t *from=flow->rows+(size_t)source*flow->basis.fn_words;
    for (uint32_t w=0;w<flow->basis.fn_words;++w) {
        if (!xir_compile_work(flow->owner->work,3)) return XR_XIR_BUDGET;
        uint64_t joined=to[w]|from[w];
        if (joined!=to[w]) flow->changed=true;
        to[w]=joined;
    }
    return XR_XIR_OK;
}

/* Opaque masks are origin facts, not the target's physical type identity.
 * Carrying them in the fixed basis prevents a temporary None or a narrower
 * receiver descriptor from erasing a genuine producer UNKNOWN contribution. */
static XrXirStatus effect_invocation_opaque(EffectInvocationFlow *flow,
    uint32_t value,bool force_unknown) {
    if (value>=flow->values) return XR_XIR_BAD_VALUE;
    const XrXirTypeNode *signature=xr_xir_callable_signature(flow->owner->module->types,
        xr_xir_operand_type(flow->function,value));
    if (!signature) return XR_XIR_OK;
    uint32_t mask=0;
    XrXirStatus status=effect_invocation_bounds_mask(flow->owner->work,flow->owner->declared,
        flow->owner->nodes[flow->node].body,value,&mask);
    if (status!=XR_XIR_OK) return status;
    uint32_t opaque=flow->owner->site_count+flow->basis.parameters;
    status=effect_invocation_origin_bit(flow,value,opaque);
    if (status==XR_XIR_OK && (mask&XR_XIR_CALLABLE_ROOT_REQUIRED))
        status=effect_invocation_origin_bit(flow,value,opaque+1);
    if (status==XR_XIR_OK && (force_unknown || (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)))
        status=effect_invocation_origin_bit(flow,value,opaque+2);
    return status;
}

static XrXirStatus effect_invocation_ref_target(EffectInvocationFlow *flow,
    uint32_t instruction,bool *authentic) {
    const XrXirModule *module=flow->owner->module;
    const XrXirInstruction *op=&flow->function->instructions[instruction];
    const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,op->type);
    *authentic=false;
    if (!signature || op->immediate<0 || (uint64_t)op->immediate>=module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *target=&module->functions[op->immediate];
    if (op->args[1]>target->parameter_count ||
        signature->parameter_count!=target->parameter_count-op->args[1] ||
        op->args[0]>flow->function->operand_count ||
        op->args[1]>flow->function->operand_count-op->args[0] ||
        (op->args[1] && !flow->function->operands)) return XR_XIR_BAD_TYPE;
    for (uint32_t p=0;p<signature->parameter_count;++p) {
        if (!xir_compile_work(flow->owner->work,2)) return XR_XIR_BUDGET;
        const XrXirCallableParameter *parameter=&signature->parameters[p];
        if (!xr_xir_callable_parameter_storage_valid(module->types,parameter)) return XR_XIR_BAD_TYPE;
        if (xr_xir_type_span(module->types,parameter->type) ||
            xr_xir_type_span(module->types,target->parameters[p+op->args[1]])) return XR_XIR_OK;
        if (parameter->mode==XR_PARAM_REF) {
            if (op->args[1]) return XR_XIR_BAD_TYPE;
            if (xr_xir_cell_provenance_role(flow->owner->effects->cells,
                    (uint32_t)op->immediate,p)!=XR_XIR_CELL_PROOF_SCOPED_REF) return XR_XIR_OK;
        }
    }
    if (xr_xir_type_span(module->types,signature->result) ||
        xr_xir_type_span(module->types,target->result)) return XR_XIR_OK;
    XrXirTypeMatchScratch scratch={flow->owner->work->resources,NULL};
    XrXirStatus status=xr_xir_compile_type_substitution_matches_between_scratch(flow->owner->work,
        module->types,module->types,NULL,0,signature->result,target->result,&scratch);
    for (uint32_t p=0;p<signature->parameter_count && status==XR_XIR_OK;++p)
        status=xr_xir_compile_type_substitution_matches_between_scratch(flow->owner->work,
            module->types,module->types,NULL,0,signature->parameters[p].type,target->parameters[p+op->args[1]],&scratch);
    for (uint32_t p=0;p<op->args[1] && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(flow->owner->work,2)) { status=XR_XIR_BUDGET;break; }
        uint32_t value=flow->function->operands[op->args[0]+p];
        if (value>=flow->values) { status=XR_XIR_BAD_VALUE;break; }
        if (xr_xir_type_span(module->types,target->parameters[p])) {
            xr_xir_type_match_scratch_free(&scratch);return XR_XIR_OK;
        }
        status=effect_invocation_value_matches(flow->owner,target->parameters[p],
            xr_xir_operand_type(flow->function,value),&scratch);
    }
    xr_xir_type_match_scratch_free(&scratch);
    if (status==XR_XIR_OK) *authentic=true;
    return status;
}

static XrXirStatus effect_invocation_site(EffectInvocationFlow *flow,
    uint32_t instruction,uint32_t *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    uint32_t body=flow->owner->nodes[flow->node].body,selected=UINT32_MAX;
    for (uint32_t s=0;s<flow->owner->site_count;++s) {
        if (!xir_compile_work(flow->owner->work,2)) return XR_XIR_BUDGET;
        const EffectInvocationSite *site=&flow->owner->sites[s];
        if (site->function!=body || site->instruction!=instruction) continue;
        if (selected!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        selected=s;
    }
    if (selected==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    *output=selected;return XR_XIR_OK;
}

/* A captured descriptor carries a flat, finite relation for every original
 * producer site. It does not recursively copy another descriptor's captures.
 * Equal producer sites encountered through PHI or recursion only OR rows. */
static XrXirStatus effect_invocation_capture(EffectInvocationFlow *flow,
    uint32_t instruction) {
    uint32_t destination=flow->function->parameter_count+instruction;
    if (!flow->certified[destination]) return XR_XIR_OK;
    uint32_t selected=UINT32_MAX;
    XrXirStatus status=effect_invocation_site(flow,instruction,&selected);
    if (status!=XR_XIR_OK) return status;
    const EffectInvocationSite *site=&flow->owner->sites[selected];
    const XrXirInstruction *op=&flow->function->instructions[instruction];
    const XrXirFunction *target=&flow->owner->module->functions[site->target];
    if (site->captures!=op->args[1] || site->target!=(uint32_t)op->immediate ||
        site->binding>flow->owner->binding_count ||
        site->captures>flow->owner->binding_count-site->binding) return XR_XIR_BAD_STRUCTURE;
    status=effect_invocation_origin_bit(flow,destination,selected);
    uint64_t *descriptor=flow->rows+(size_t)destination*flow->basis.fn_words;
    uint32_t binding_words=flow->basis.origin_words+flow->basis.cell_words;
    for (uint32_t p=0;p<site->captures && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(flow->owner->work,2)) return XR_XIR_BUDGET;
        uint32_t value=flow->function->operands[op->args[0]+p];
        if (value>=flow->values) return XR_XIR_BAD_VALUE;
        uint64_t *binding=descriptor+flow->basis.origin_words+
            (size_t)(site->binding+p)*binding_words;
        if (xr_xir_type_is_cell(flow->owner->module->types,target->parameters[p])) {
            uint64_t *cell=binding+flow->basis.origin_words;
            /* Capture Cell facts come only from the shared real producer proof. */
            status=effect_invocation_cell(flow,value,cell,&flow->changed);
        } else if (xr_xir_callable_signature(flow->owner->module->types,target->parameters[p])) {
            const uint64_t *source=flow->rows+(size_t)value*flow->basis.fn_words;
            for (uint32_t w=0;w<flow->basis.origin_words;++w) {
                if (!xir_compile_work(flow->owner->work,3)) return XR_XIR_BUDGET;
                uint64_t joined=binding[w]|source[w];
                if (joined!=binding[w]) flow->changed=true;
                binding[w]=joined;
            }
            for (uint32_t w=flow->basis.origin_words;w<flow->basis.fn_words;++w) {
                if (!xir_compile_work(flow->owner->work,3)) return XR_XIR_BUDGET;
                uint64_t joined=descriptor[w]|source[w];
                if (joined!=descriptor[w]) flow->changed=true;
                descriptor[w]=joined;
            }
        }
    }
    return status;
}

static XrXirStatus effect_invocation_fixed(EffectInvocationFlow *flow,uint32_t value) {
    if (value>=flow->values) return XR_XIR_BAD_VALUE;
    if (!xir_compile_work(flow->owner->work,2)) return XR_XIR_BUDGET;
    if (!flow->fixed[value]) { flow->fixed[value]=1;flow->changed=true; }
    return effect_invocation_opaque(flow,value,false);
}

static XrXirStatus effect_invocation_barriers(EffectInvocationFlow *flow) {
    const XrXirModule *module=flow->owner->module;
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(flow->owner->work,1)) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&flow->function->instructions[i];
        if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        bool store=op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_CELL_WRITE ||
            op->op==XR_XIR_CELL_LOCAL_WRITE || op->op==XR_XIR_PLACE_WRITE ||
            op->op==XR_XIR_SLOT_INIT || op->op==XR_XIR_SLOT_GROUP_INIT ||
            op->op==XR_XIR_SLOT_STORE || op->op==XR_XIR_ARRAY_NEW || op->op==XR_XIR_ARRAY_SET ||
            op->op==XR_XIR_ARRAY_PUSH || op->op==XR_XIR_STRUCT_NEW || op->op==XR_XIR_STRUCT_SET ||
            op->op==XR_XIR_CLASS_NEW || op->op==XR_XIR_CLASS_SET || op->op==XR_XIR_ENUM_NEW ||
            op->op==XR_XIR_TUPLE_NEW || op->op==XR_XIR_NULLABLE_SOME;
        if (!store) continue;
        bool table=xr_xir_op_uses_operand_table(op->op);
        uint32_t count=table ? op->args[1] : effect_parameter_operands[op->op];
        if (op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE) {
            XrXirType cell=op->op==XR_XIR_CELL_NEW ? op->type :
                xr_xir_operand_type(flow->function,op->args[0]);
            count=xr_xir_cell_payload_operands(module->types,cell,op->op);
        } else if (op->op==XR_XIR_SLOT_INIT || op->op==XR_XIR_SLOT_STORE) {
            if (!module->declarations) return XR_XIR_BAD_STRUCTURE;
            count=xr_xir_slot_payload_operands(module->declarations->slots,
                module->declarations->slot_count,op);
        }
        if (table && ((count && !flow->function->operands) ||
            op->args[0]>flow->function->operand_count ||
            count>flow->function->operand_count-op->args[0])) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a=0;a<count;++a) {
            if (!xir_compile_work(flow->owner->work,1)) return XR_XIR_BUDGET;
            uint32_t value=table ? flow->function->operands[op->args[0]+a] : op->args[a];
            XrXirStatus status=effect_invocation_fixed(flow,value);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_transfer(EffectInvocationFlow *flow,
    uint32_t destination,uint32_t source) {
    XrXirStatus status=effect_invocation_origin_join(flow,destination,source);
    if (status!=XR_XIR_OK) return status;
    if (flow->fixed[source]) return effect_invocation_fixed(flow,destination);
    return XR_XIR_OK;
}

/* Returned identities supplement the original opaque advertisement. Only
 * ordinary NONE return signatures participate; REF/UNKNOWN/fixed barriers
 * retain their original bounds and cannot acquire a permission here. */
static XrXirStatus effect_invocation_return_origins(EffectInvocationFlow *flow,
    uint32_t instruction) {
    EffectInvocationOwner *owner=flow->owner;
    if (!owner->producer_enabled) return XR_XIR_OK;
    const XrXirInstruction *op=&flow->function->instructions[instruction];
    uint32_t frontier=instruction,destination=flow->function->parameter_count+instruction;
    if (op->op==XR_XIR_INVOKE_RESULT) {
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        frontier=(uint32_t)op->immediate;
    } else if (op->op!=XR_XIR_CALL && op->op!=XR_XIR_CALL_DEFAULT && op->op!=XR_XIR_CALL_INDIRECT)
        return XR_XIR_OK;
    if (!xr_xir_callable_signature(owner->module->types,op->type)) return XR_XIR_OK;
    for (uint32_t e=owner->nodes[flow->node].edge_head;e!=UINT32_MAX;e=owner->edges[e].next) {
        if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
        if (e>=owner->edge_count || owner->edges[e].caller!=flow->node ||
            owner->edges[e].target>=owner->count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationEdge edge=owner->edges[e];
        if (edge.latent || edge.instruction!=frontier) continue;
        const EffectInvocationNode *child=&owner->nodes[edge.target];
        const XrXirFunction *target=&owner->module->functions[child->body];
        uint64_t returned=(uint64_t)target->parameter_count+target->instruction_count;
        for (uint32_t p=0;p<child->producer_count;++p) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            if (child->producers[p].value!=returned) continue;
            XrXirStatus status=effect_invocation_origin_bit(flow,destination,child->producers[p].site);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_origins_step(EffectInvocationFlow *flow,
    uint32_t instruction) {
    if (!xir_compile_work(flow->owner->work,1)) return XR_XIR_BUDGET;
    const XrXirInstruction *op=&flow->function->instructions[instruction];
    uint32_t destination=flow->function->parameter_count+instruction;
    switch (op->op) {
    case XR_XIR_CALL: case XR_XIR_CALL_DEFAULT: case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_RESULT:
        return effect_invocation_return_origins(flow,instruction);
    case XR_XIR_FUNCTION_REF:
        return effect_invocation_capture(flow,instruction);
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_NEW:
    case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_READ:
        return effect_invocation_transfer(flow,destination,op->args[0]);
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_WRITE:
        return effect_invocation_transfer(flow,op->args[0],op->args[1]);
    case XR_XIR_PHI:
        if (!op->args[1] || op->args[1]%2 || !flow->function->operands ||
            op->args[0]>flow->function->operand_count ||
            op->args[1]>flow->function->operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a=1;a<op->args[1];a+=2) {
            XrXirStatus status=effect_invocation_transfer(flow,destination,
                flow->function->operands[op->args[0]+a]);
            if (status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    default: return XR_XIR_OK;
    }
}

static XrXirStatus effect_invocation_origins(EffectInvocationFlow *flow) {
    EffectInvocationOwner *owner=flow->owner;
    if (flow->node>=owner->count) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationNode identity=owner->nodes[flow->node];
    flow->function=&owner->module->functions[identity.body];
    XrXirStatus status=effect_invocation_basis(owner,identity.root,&flow->basis);
    if (status!=XR_XIR_OK) return status;
    uint64_t values=(uint64_t)flow->function->parameter_count+flow->function->instruction_count;
    uint64_t words=values*flow->basis.fn_words;
    if (values>UINT32_MAX || words>SIZE_MAX/sizeof(*flow->rows) ||
        values>(SIZE_MAX-words*sizeof(*flow->rows))/2) return XR_XIR_BUDGET;
    size_t bytes=(size_t)(words*sizeof(*flow->rows)+values*2);
    flow->memory=xir_compile_calloc(owner->work,1,bytes,&status);
    if (!flow->memory) return status;
    flow->rows=flow->memory;flow->fixed=(uint8_t *)(flow->rows+(size_t)words);
    flow->certified=flow->fixed+(size_t)values;
    flow->values=(uint32_t)values;
    for (uint32_t p=0;p<flow->function->parameter_count;++p) {
        if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*flow->rows)))
            return XR_XIR_BUDGET;
        memcpy(flow->rows+(size_t)p*flow->basis.fn_words,
            identity.input+(size_t)p*flow->basis.row_words,
            (size_t)flow->basis.fn_words*sizeof(*flow->rows));
    }
    status=effect_invocation_barriers(flow);
    for (uint32_t i=0;i<flow->function->instruction_count && status==XR_XIR_OK;++i) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&flow->function->instructions[i];
        uint32_t destination=flow->function->parameter_count+i;
        if (!xr_xir_callable_signature(owner->module->types,op->type)) continue;
        if (op->op==XR_XIR_FUNCTION_REF) {
            bool authentic=false;
            status=effect_invocation_ref_target(flow,i,&authentic);
            if (status==XR_XIR_OK && authentic) flow->certified[destination]=1;
            else if (status==XR_XIR_OK) status=effect_invocation_opaque(flow,destination,true);
        } else if (op->op!=XR_XIR_COPY && op->op!=XR_XIR_LOCAL_NEW &&
            op->op!=XR_XIR_LOCAL_READ && op->op!=XR_XIR_PHI &&
            op->op!=XR_XIR_SCALAR_COPY && op->op!=XR_XIR_OWNED_RETAIN &&
            op->op!=XR_XIR_SCALAR_LOCAL_NEW && op->op!=XR_XIR_SCALAR_LOCAL_READ &&
            op->op!=XR_XIR_OWNED_LOCAL_NEW && op->op!=XR_XIR_OWNED_LOCAL_READ)
            status=effect_invocation_opaque(flow,destination,false);
    }
    flow->changed=true;
    while (status==XR_XIR_OK && flow->changed) {
        flow->changed=false;
        for (uint32_t i=0;i<flow->function->instruction_count && status==XR_XIR_OK;++i)
            status=effect_invocation_origins_step(flow,i);
    }
    /* Empty cyclic carrier rows have no producer certificate. Add UNKNOWN
     * before any execution result can consume the internal origin bottom. */
    for (uint32_t v=0;v<flow->values && status==XR_XIR_OK;++v) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        if (!xr_xir_callable_signature(owner->module->types,xr_xir_operand_type(flow->function,v))) continue;
        bool present=false;
        for (uint32_t w=0;w<flow->basis.origin_words;++w) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            if (flow->rows[(size_t)v*flow->basis.fn_words+w]) present=true;
        }
        if (!present) status=effect_invocation_opaque(flow,v,true);
    }
    while (status==XR_XIR_OK && flow->changed) {
        flow->changed=false;
        for (uint32_t i=0;i<flow->function->instruction_count && status==XR_XIR_OK;++i)
            status=effect_invocation_origins_step(flow,i);
    }
    return status;
}

/* This operation consumes the common Cell proof. It does not infer a second
 * set of Cell roles or origins: each compact provider dependency is projected
 * through the already-owned physical input row of this exact equation. */
static XrXirStatus effect_invocation_cell(EffectInvocationFlow *flow,
    uint32_t value,uint64_t *output,bool *changed) {
    EffectInvocationOwner *owner=flow->owner;
    if (flow->node>=owner->count || !output || value>=flow->values)
        return XR_XIR_BAD_STRUCTURE;
    EffectInvocationNode identity=owner->nodes[flow->node];
    XrXirCellOriginView view={0};
    XrXirStatus status=xr_xir_compile_cell_origin_view(owner->work,owner->effects->cells,
        identity.body,value,&view);
    if (status!=XR_XIR_OK) return status;
    if (view.word_count!=((uint64_t)view.parameter_count+63)/64 ||
        (!!view.dependencies!=!!view.word_count) ||
        (!!view.parameters!=!!view.parameter_count) ||
        (view.intrinsic_mask&~(XR_XIR_CELL_ACCESS_ROOT|XR_XIR_CELL_ACCESS_UNKNOWN)))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
    uint32_t known=flow->basis.parameters+2;
    uint64_t known_bit=UINT64_C(1)<<(known%64);
    if (changed && !(output[known/64]&known_bit)) *changed=true;
    output[known/64]|=known_bit;
    if (view.intrinsic_mask&XR_XIR_CELL_ACCESS_ROOT) {
        uint64_t root_bit=UINT64_C(1)<<(flow->basis.parameters%64);
        if (changed && !(output[flow->basis.parameters/64]&root_bit)) *changed=true;
        output[flow->basis.parameters/64]|=root_bit;
    }
    if (view.intrinsic_mask&XR_XIR_CELL_ACCESS_UNKNOWN) {
        uint32_t bit=flow->basis.parameters+1;
        uint64_t unknown_bit=UINT64_C(1)<<(bit%64);
        if (changed && !(output[bit/64]&unknown_bit)) *changed=true;
        output[bit/64]|=unknown_bit;
    }
    for (uint32_t bit=0;bit<view.parameter_count;++bit) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (!(view.dependencies[bit/64]&(UINT64_C(1)<<(bit%64)))) continue;
        uint32_t parameter=view.parameters[bit];
        if (parameter>=flow->function->parameter_count ||
            !xr_xir_type_is_cell(owner->module->types,flow->function->parameters[parameter]))
            return XR_XIR_BAD_STRUCTURE;
        const uint64_t *row=identity.input+(size_t)parameter*flow->basis.row_words+flow->basis.fn_words;
        bool present=(row[known/64]&(UINT64_C(1)<<(known%64)))!=0;
        for (uint32_t w=0;w<flow->basis.cell_words;++w) {
            if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
            uint64_t joined=output[w]|row[w];
            if (changed && joined!=output[w]) *changed=true;
            output[w]=joined;
        }
        /* An unbound physical formal cannot be interpreted as an owned root. */
        if (!present) {
            uint32_t unknown=flow->basis.parameters+1;
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            uint64_t unknown_bit=UINT64_C(1)<<(unknown%64);
            if (changed && !(output[unknown/64]&unknown_bit)) *changed=true;
            output[unknown/64]|=unknown_bit;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_cell_consume(EffectInvocationFlow *flow,
    const uint64_t *row,bool *changed) {
    EffectInvocationOwner *owner=flow->owner;
    if (!row || !changed || flow->node>=owner->count) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationNode *node=&owner->nodes[flow->node];
    uint32_t mask=0;
    if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
    uint32_t known=flow->basis.parameters+2;
    if (!(row[known/64]&(UINT64_C(1)<<(known%64))))
        mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    if (row[flow->basis.parameters/64]&(UINT64_C(1)<<(flow->basis.parameters%64)))
        mask|=XR_XIR_CALLABLE_ROOT_REQUIRED;
    uint32_t unknown=flow->basis.parameters+1;
    if (row[unknown/64]&(UINT64_C(1)<<(unknown%64))) mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    if ((node->intrinsic_mask|mask)!=node->intrinsic_mask) *changed=true;
    node->intrinsic_mask|=mask;
    if (flow->instruction>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t unknown_bit=0;unknown_bit<2;++unknown_bit) {
        uint32_t flag=unknown_bit?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED;
        if (!(mask&flag)) continue;
        XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_CELL_ACCESS,flow->instruction,UINT32_MAX,
            flow->function->instructions[flow->instruction].args[0],0};
        XrXirStatus status=effect_invocation_local(owner,flow->node,unknown_bit,witness);
        if (status!=XR_XIR_OK) return status;
    }
    for (uint32_t p=0;p<flow->basis.parameters;++p) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        uint64_t bit=UINT64_C(1)<<(p%64);
        if (!(row[p/64]&bit)) continue;
        if (!xr_xir_type_is_cell(owner->module->types,owner->module->functions[node->root].parameters[p]))
            return XR_XIR_BAD_STRUCTURE;
        if (!(node->cells[p/64]&bit)) { node->cells[p/64]|=bit;*changed=true; }
        XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_CELL_PARAMETER,flow->instruction,UINT32_MAX,p,0};
        XrXirStatus status=effect_invocation_local(owner,flow->node,flow->basis.parameters+p+2,witness);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
