/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_class_flow.inc.c - Complete Class sources on actual equation keys
 *
 * KEY CONCEPT:
 *   Class facts name true SSA values on the existing equation owner. Pure
 *   facts require every real source and return, with finite descending ranks.
 */
typedef struct EffectInvocationClassSources {
    uint32_t mask;
    uint64_t maximum;
    bool complete;
} EffectInvocationClassSources;

static XrXirStatus effect_invocation_coverage_input(EffectInvocationFlow *flow,
    uint32_t edge,bool *matched);

static XrXirStatus effect_invocation_class_input(const EffectInvocationFlow *flow,
    const uint64_t *input,uint32_t *output) {
    if (!input || !output || flow->basis.row_words!=
        (uint64_t)flow->basis.fn_words+flow->basis.cell_words) return XR_XIR_BAD_STRUCTURE;
    uint32_t mask=0;
    for (uint32_t w=0;w<flow->basis.row_words;++w) {
        if (!xir_compile_work(flow->owner->work,3)) return XR_XIR_BUDGET;
        uint64_t permitted=0;
        if (w>=flow->basis.fn_words) {
            uint32_t c=w-flow->basis.fn_words;
            for (uint32_t bit=0;bit<3;++bit) {
                uint32_t index=flow->basis.parameters+bit;
                if (c!=index/64) continue;
                uint64_t value=UINT64_C(1)<<(index%64);
                permitted|=value;
                if (input[w]&value) mask|=bit==0 ? EFFECT_CLASS_ROOT :
                    bit==1 ? EFFECT_CLASS_UNKNOWN : EFFECT_CLASS_KNOWN;
            }
        }
        if (input[w]&~permitted) return XR_XIR_BAD_STRUCTURE;
    }
    if (!(mask&EFFECT_CLASS_KNOWN)) return XR_XIR_BAD_STRUCTURE;
    *output=mask;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_class_encode(EffectInvocationFlow *flow,
    uint32_t mask,uint64_t *output) {
    if (!output || !mask || (mask&~EFFECT_CLASS_VALID) || !(mask&EFFECT_CLASS_KNOWN))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->owner->work,3)) return XR_XIR_BUDGET;
    for (uint32_t bit=0;bit<3;++bit) {
        uint32_t flag=bit==0?EFFECT_CLASS_ROOT:bit==1?EFFECT_CLASS_UNKNOWN:EFFECT_CLASS_KNOWN;
        if (!(mask&flag)) continue;
        uint32_t index=flow->basis.parameters+bit;
        output[index/64]|=UINT64_C(1)<<(index%64);
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_class_word(EffectInvocationFlow *flow,
    uint32_t value,uint64_t *output) {
    if (!output || value>=flow->values) return XR_XIR_BAD_STRUCTURE;
    uint32_t at=0;EffectInvocationOwner *owner=flow->owner;
    XrXirStatus status=effect_invocation_class_find(owner->work,&owner->nodes[flow->node],value,&at);
    if (status!=XR_XIR_OK) return status;
    const EffectInvocationClassFact fact=owner->nodes[flow->node].classes[at];
    if (!fact.mask) return XR_XIR_UNRESOLVED;
    if ((fact.mask&~EFFECT_CLASS_VALID) || !(fact.mask&EFFECT_CLASS_KNOWN) ||
        (fact.mask==EFFECT_CLASS_KNOWN ? !fact.rank : fact.rank!=0)) return XR_XIR_BAD_STRUCTURE;
    return effect_invocation_class_encode(flow,fact.mask,output);
}

static XrXirStatus effect_invocation_class_source(EffectInvocationFlow *flow,
    uint32_t source,EffectInvocationClassSources *sources) {
    if (!sources || source>=flow->values) return XR_XIR_BAD_STRUCTURE;
    XrXirType actual_type=0;
    XrXirStatus type_status=effect_invocation_flow_type(flow,source,&actual_type);
    if (type_status!=XR_XIR_OK) return type_status;
    if (!xr_xir_type_is_class(flow->body.domain.types,actual_type)) return XR_XIR_BAD_STRUCTURE;
    uint32_t at=0;EffectInvocationOwner *owner=flow->owner;
    XrXirStatus status=effect_invocation_class_find(owner->work,&owner->nodes[flow->node],source,&at);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
    EffectInvocationClassFact fact=owner->nodes[flow->node].classes[at];
    if (!fact.mask) { sources->complete=false;return XR_XIR_OK; }
    if ((fact.mask&~EFFECT_CLASS_VALID) || !(fact.mask&EFFECT_CLASS_KNOWN) ||
        (fact.mask==EFFECT_CLASS_KNOWN ? !fact.rank : fact.rank!=0)) return XR_XIR_BAD_STRUCTURE;
    sources->mask|=fact.mask;
    if (fact.rank>sources->maximum) sources->maximum=fact.rank;
    return XR_XIR_OK;
}

/* Current complete inputs, not historical smaller environments, determine
 * a direct result. Indirect opaque advertisements remain explicit unknown. */
static XrXirStatus effect_invocation_class_call(EffectInvocationFlow *flow,
    uint32_t instruction,EffectInvocationClassSources *sources) {
    EffectInvocationOwner *owner=flow->owner;
    XrXirInstruction body_op_1;const XrXirInstruction *value=NULL;
    XrXirStatus body_read_1=effect_invocation_flow_instruction(flow,instruction,&body_op_1,&value);
    if (body_read_1!=XR_XIR_OK) return body_read_1;
    uint32_t frontier=instruction;
    if (value->op==XR_XIR_INVOKE_RESULT) {
        if (value->immediate<0 || (uint64_t)value->immediate>=flow->function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        frontier=(uint32_t)value->immediate;
    }
    XrXirInstruction body_op_2;const XrXirInstruction *op=NULL;
    XrXirStatus body_read_2=effect_invocation_flow_instruction(flow,frontier,&body_op_2,&op);
    if (body_read_2!=XR_XIR_OK) return body_read_2;
    if (xr_xir_type_span(flow->body.domain.types,value->type)) {
        sources->mask=EFFECT_CLASS_KNOWN|EFFECT_CLASS_UNKNOWN;return XR_XIR_OK;
    }
    if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) {
        sources->mask=EFFECT_CLASS_KNOWN|EFFECT_CLASS_UNKNOWN;return XR_XIR_OK;
    }
    if (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE &&
        op->op!=XR_XIR_CALL_DEFAULT && op->op!=XR_XIR_INVOKE_DEFAULT)
        return XR_XIR_BAD_STRUCTURE;
    if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE) {
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->body.domain.function_count)
            return XR_XIR_BAD_STRUCTURE;
        EffectBodyFunctionHead target_head={0};
        XrXirStatus head_status=effect_invocation_flow_head(flow,(uint32_t)op->immediate,&target_head);
        if (head_status!=XR_XIR_OK) return head_status;
        const EffectBodyFunctionHead *target=&target_head;
        for (uint32_t p=0;p<target->parameter_count;++p) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            if (xr_xir_type_span(flow->body.domain.types,target->parameters[p])) {
                sources->mask=EFFECT_CLASS_KNOWN|EFFECT_CLASS_UNKNOWN;return XR_XIR_OK;
            }
        }
    }
    uint32_t matches=0,walked=0;
    for (uint32_t e=owner->nodes[flow->node].edge_head;e!=UINT32_MAX;e=owner->edges[e].next) {
        if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
        if (e>=owner->edge_count || ++walked>owner->edge_count) return XR_XIR_BAD_STRUCTURE;
        EffectInvocationEdge edge=owner->edges[e];
        if (edge.instruction!=frontier || edge.latent) continue;
        bool matched=false;XrXirStatus status=effect_invocation_coverage_input(flow,e,&matched);
        if (status==XR_XIR_UNRESOLVED) { sources->complete=false;continue; }
        if (status!=XR_XIR_OK) return status;
        if (!matched) continue;
        if (++matches!=1 || edge.target>=owner->count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationNode *target=&owner->nodes[edge.target];
        EffectBodyFunctionHead target_head={0};
        status=effect_invocation_flow_head(flow,target->body,&target_head);
        if (status!=XR_XIR_OK) return status;
        if (!xr_xir_type_is_class(flow->body.domain.types,target_head.result)) return XR_XIR_BAD_STRUCTURE;
        if (!target->class_return_mask) { sources->complete=false;continue; }
        if ((target->class_return_mask&~EFFECT_CLASS_VALID) ||
            !(target->class_return_mask&EFFECT_CLASS_KNOWN) ||
            (target->class_return_mask==EFFECT_CLASS_KNOWN ?
                !target->class_return_rank : target->class_return_rank!=0)) return XR_XIR_BAD_STRUCTURE;
        sources->mask|=target->class_return_mask;
        if (target->class_return_rank>sources->maximum) sources->maximum=target->class_return_rank;
    }
    if (!matches) sources->complete=false;
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_class_writes(EffectInvocationFlow *flow,
    uint32_t source,EffectInvocationClassSources *sources) {
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(flow->owner->work,1)) return XR_XIR_BUDGET;
        XrXirInstruction body_op_3;const XrXirInstruction *write=NULL;
        XrXirStatus body_read_3=effect_invocation_flow_instruction(flow,i,&body_op_3,&write);
        if (body_read_3!=XR_XIR_OK) return body_read_3;
        if (write->op!=XR_XIR_LOCAL_WRITE && write->op!=XR_XIR_SCALAR_LOCAL_WRITE &&
            write->op!=XR_XIR_OWNED_LOCAL_WRITE) continue;
        if (write->args[0]!=source) continue;
        XrXirStatus status=effect_invocation_class_source(flow,write->args[1],sources);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_class_value(EffectInvocationFlow *flow,
    uint32_t value,EffectInvocationClassSources *sources) {
    EffectInvocationOwner *owner=flow->owner;
    if (!sources || value>=flow->values) return XR_XIR_BAD_STRUCTURE;
    *sources=(EffectInvocationClassSources){.complete=true};
    if (value<flow->function->parameter_count) {
        XrXirStatus status=effect_invocation_class_input(flow,
            owner->nodes[flow->node].input+(size_t)value*flow->basis.row_words,&sources->mask);
        return status;
    }
    uint32_t i=value-flow->function->parameter_count;
    XrXirInstruction body_op_4;const XrXirInstruction *op=NULL;
    XrXirStatus body_read_4=effect_invocation_flow_instruction(flow,i,&body_op_4,&op);
    if (body_read_4!=XR_XIR_OK) return body_read_4;
    switch (op->op) {
    case XR_XIR_CLASS_NEW: sources->mask=EFFECT_CLASS_KNOWN;return XR_XIR_OK;
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
        return effect_invocation_class_source(flow,op->args[0],sources);
    case XR_XIR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_NEW:
    case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_READ: {
        XrXirStatus status=effect_invocation_class_source(flow,op->args[0],sources);
        if (status!=XR_XIR_OK) return status;
        uint32_t local=op->op==XR_XIR_LOCAL_NEW || op->op==XR_XIR_SCALAR_LOCAL_NEW ||
            op->op==XR_XIR_OWNED_LOCAL_NEW ? value : op->args[0];
        return effect_invocation_class_writes(flow,local,sources);
    }
    case XR_XIR_PHI:
        if (!op->args[1] || op->args[0]>flow->function->operand_count ||
            op->args[1]>flow->function->operand_count-op->args[0] || !flow->function->operands)
            return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p=0;p<op->args[1];p+=2) {
            if (p+1>=op->args[1]) return XR_XIR_BAD_STRUCTURE;
            XrXirStatus status=effect_invocation_class_source(flow,
                flow->function->operands[op->args[0]+p+1],sources);
            if (status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    case XR_XIR_CALL: case XR_XIR_CALL_DEFAULT: case XR_XIR_CALL_INDIRECT:
    case XR_XIR_INVOKE_RESULT:
        return effect_invocation_class_call(flow,i,sources);
    default:
        /* Cell payloads, aggregate extraction and unbound carriers do not
         * prove a Class identity's complete publication history. */
        sources->mask=EFFECT_CLASS_KNOWN|EFFECT_CLASS_UNKNOWN;return XR_XIR_OK;
    }
}

static XrXirStatus effect_invocation_class_return(EffectInvocationFlow *flow,
    uint64_t limit,bool *changed) {
    EffectInvocationOwner *owner=flow->owner;
    if (!xr_xir_type_is_class(flow->body.domain.types,flow->function->result)) return XR_XIR_OK;
    EffectInvocationClassSources sources={.complete=true};uint32_t count=0;
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        XrXirInstruction body_op_5;const XrXirInstruction *op=NULL;
        XrXirStatus body_read_5=effect_invocation_flow_instruction(flow,i,&body_op_5,&op);
        if (body_read_5!=XR_XIR_OK) return body_read_5;
        if (op->op!=XR_XIR_RETURN) continue;
        ++count;XrXirStatus status=effect_invocation_class_source(flow,op->args[0],&sources);
        if (status!=XR_XIR_OK) return status;
    }
    if (!count) sources.mask=EFFECT_CLASS_KNOWN|EFFECT_CLASS_UNKNOWN;
    if (!sources.complete) return XR_XIR_OK;
    uint64_t rank=sources.mask==EFFECT_CLASS_KNOWN ? sources.maximum+1 : 0;
    if (rank>limit || (sources.mask==EFFECT_CLASS_KNOWN && !rank)) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationNode *node=&owner->nodes[flow->node];
    EffectInvocationClassFact fact={.mask=node->class_return_mask,.rank=node->class_return_rank};
    XrXirStatus status=effect_invocation_class_fact_join(owner,&fact,sources.mask,rank,limit,changed);
    if (status==XR_XIR_OK) {
        node->class_return_mask=fact.mask;node->class_return_rank=fact.rank;
    }
    return status;
}

static XrXirStatus effect_invocation_class_step(EffectInvocationFlow *flow,
    bool *changed) {
    if (!changed || flow->node>=flow->owner->count) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationOwner *owner=flow->owner;
    if (!owner->class_enabled) return XR_XIR_OK;
    uint64_t limit=0;XrXirStatus status=effect_invocation_class_rank_limit_count(owner,flow->body.domain.function_count,&limit);
    if (status!=XR_XIR_OK) return status;
    bool again=true;
    while (again) {
        again=false;
        uint32_t count=owner->nodes[flow->node].class_count;
        for (uint32_t c=0;c<count;++c) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            uint32_t value=owner->nodes[flow->node].classes[c].value;
            EffectInvocationClassSources sources={0};
            status=effect_invocation_class_value(flow,value,&sources);
            if (status!=XR_XIR_OK) return status;
            if (!sources.complete || !sources.mask) continue;
            uint64_t rank=sources.mask==EFFECT_CLASS_KNOWN ? sources.maximum+1 : 0;
            if (rank>limit || (sources.mask==EFFECT_CLASS_KNOWN && !rank)) return XR_XIR_BAD_STRUCTURE;
            status=effect_invocation_class_fact_join(owner,&owner->nodes[flow->node].classes[c],
                sources.mask,rank,limit,&again);
            if (status!=XR_XIR_OK) return status;
        }
        if (again) *changed=true;
    }
    return effect_invocation_class_return(flow,limit,changed);
}

/* Once the existing queue is quiet, an unrooted Class cycle retains UNKNOWN.
 * Filling residuals wakes that same queue. No bottom may survive admission or
 * become an actual argument environment, a returned identity or a permission.
 */
static XrXirStatus effect_invocation_class_residual(EffectInvocationOwner *owner,
    bool *again) {
    if (!again || !owner || owner->pending) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
    *again=false;if (!owner->class_enabled) return XR_XIR_OK;
    uint64_t limit=0;XrXirStatus status=effect_invocation_class_rank_limit(owner,&limit);
    for (uint32_t n=0;status==XR_XIR_OK && n<owner->count;++n) {
        EffectInvocationNode *node=&owner->nodes[n];bool changed=false;
        for (uint32_t c=0;c<node->class_count;++c) {
            if (!xir_compile_work(owner->work,1)) { status=XR_XIR_BUDGET;break; }
            if (node->classes[c].mask) continue;
            status=effect_invocation_class_fact_join(owner,&node->classes[c],
                EFFECT_CLASS_KNOWN|EFFECT_CLASS_UNKNOWN,0,limit,&changed);
            if (status!=XR_XIR_OK) break;
        }
        EffectBodyFunctionHead head={0};
        if (status==XR_XIR_OK) status=effect_invocation_head(owner,node->body,&head);
        if (status==XR_XIR_OK && xr_xir_type_is_class(owner->body.domain.types,head.result) &&
            !node->class_return_mask && !head.instruction_count) {
            if (!xir_compile_work(owner->work,2)) status=XR_XIR_BUDGET;
            else { node->class_return_mask=EFFECT_CLASS_KNOWN|EFFECT_CLASS_UNKNOWN;changed=true; }
        }
        if (status==XR_XIR_OK && changed) {
            status=effect_invocation_enqueue(owner,n);*again=true;
        }
    }
    return status;
}
