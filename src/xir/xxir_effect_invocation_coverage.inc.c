/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_coverage.inc.c - Exhaustive actual callable return coverage
 *
 * KEY CONCEPT:
 *   A producer site proves existence. Coverage requires every real source and
 *   return exit on an exact immutable equation, with strictly descending ranks.
 *   These private facts discharge error residuals, never ROOT advertisements.
 */
typedef struct EffectInvocationCoverage {
    EffectInvocationFlow *flow;
    uint64_t *ranks;
    uint32_t *writes,*next,*matches;
    void *memory;
} EffectInvocationCoverage;

/* The input builder is shared with production invocation admission. Old
 * smaller environments remain valid edges, but cannot execute this full key. */
static XrXirStatus effect_invocation_coverage_input(EffectInvocationFlow *flow,
    uint32_t e,bool *matched) {
    const EffectInvocationRead *owner=flow->read;*matched=false;
    XrXirStatus status=effect_invocation_edge_shape_read(owner->work,owner->final,&flow->body,
        flow->body.domain.types,flow->node,e);
    if (status!=XR_XIR_OK) return status;
    EffectInvocationEdge edge=owner->final->edges[e];
    const EffectInvocationNode *target=&owner->final->nodes[edge.target];
    XrXirInstruction body_op_1;const XrXirInstruction *op=NULL;
    XrXirStatus body_read_1=effect_invocation_flow_instruction(flow,edge.instruction,&body_op_1,&op);
    if (body_read_1!=XR_XIR_OK) return body_read_1;
    EffectBodyFunctionHead target_head={0};
    status=effect_invocation_flow_head(flow,target->body,&target_head);
    if (status!=XR_XIR_OK) return status;
    if (target->parameter_count!=target_head.parameter_count) return XR_XIR_BAD_STRUCTURE;
    if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
        if (target->parameter_count || target->input) return XR_XIR_BAD_STRUCTURE;
        *matched=true;return XR_XIR_OK;
    }
    const uint64_t *closure=NULL;
    if (edge.producer!=UINT32_MAX && op->op!=XR_XIR_FUNCTION_REF) {
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->values) return XR_XIR_BAD_STRUCTURE;
        closure=effect_invocation_row(flow,(uint32_t)op->immediate);
        if (!closure) return XR_XIR_BAD_STRUCTURE;
    }
    uint64_t *input=NULL;flow->instruction=edge.instruction;
    if (op->op==XR_XIR_FUNCTION_REF)
        status=effect_invocation_latent_input(flow,edge.instruction,edge.producer,op,&input);
    else status=effect_invocation_arguments(flow,op,target->body,edge.producer,closure,&input);
    uint64_t words=(uint64_t)target->parameter_count*flow->basis.row_words;
    if (status==XR_XIR_OK && words>SIZE_MAX/sizeof(*input)) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK && words && (!input || !target->input)) status=XR_XIR_BAD_STRUCTURE;
    bool same=true;
    for (size_t w=0;status==XR_XIR_OK && w<(size_t)words;++w) {
        if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
        if (input[w]!=target->input[w]) same=false;
    }
    xr_compile_resources_free(input);if (status==XR_XIR_OK) *matched=same;return status;
}

static XrXirStatus effect_invocation_coverage_storage(EffectInvocationFlow *flow,
    EffectInvocationCoverage *coverage) {
    if (!coverage || coverage->memory || !flow || !flow->read || !flow->rows)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t rank_bytes=(uint64_t)flow->values*sizeof(uint64_t);
    uint64_t indices=(uint64_t)flow->values+flow->function->instruction_count+flow->read->final->site_count;
    if (rank_bytes>SIZE_MAX || indices>(SIZE_MAX-rank_bytes)/sizeof(uint32_t)) return XR_XIR_BUDGET;
    size_t bytes=(size_t)(rank_bytes+indices*sizeof(uint32_t));
    XrXirStatus status=XR_XIR_OK;
    void *memory=xir_compile_calloc(flow->read->work,1,bytes,&status);
    if (!memory) return status;
    coverage->flow=flow;coverage->memory=memory;coverage->ranks=memory;
    coverage->writes=(uint32_t *)((uint8_t *)memory+(size_t)rank_bytes);
    coverage->next=coverage->writes+flow->values;
    coverage->matches=coverage->next+flow->function->instruction_count;
    if (!xir_compile_work(flow->read->work,flow->values)) return XR_XIR_BUDGET;
    for (uint32_t v=0;v<flow->values;++v) coverage->writes[v]=UINT32_MAX;
    EffectBodyRecordCursor cursor;
    status=effect_body_request_sequence_begin(&flow->body,&cursor);
    if (status!=XR_XIR_OK) return status;
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
        XrXirInstruction body_op_2;const XrXirInstruction *op=NULL;
        XrXirStatus body_read_2=effect_body_request_sequence_instruction(&flow->body,
            flow->read->final->nodes[flow->node].body,i,&cursor,&body_op_2,&op);
        if (body_read_2!=XR_XIR_OK) return body_read_2;
        if (op->op!=XR_XIR_LOCAL_WRITE && op->op!=XR_XIR_SCALAR_LOCAL_WRITE &&
            op->op!=XR_XIR_OWNED_LOCAL_WRITE) continue;
        if (op->args[0]>=flow->values || op->args[1]>=flow->values) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(flow->read->work,3)) return XR_XIR_BUDGET;
        coverage->next[i]=coverage->writes[op->args[0]];coverage->writes[op->args[0]]=i;
    }
    return effect_body_request_sequence_finish(&flow->body,flow->read->final->nodes[flow->node].body,&cursor);
}

/* Every bit of the existing target superset needs its genuine producer cause.
 * Presence alone never establishes coverage: the source clauses below do. */
static XrXirStatus effect_invocation_coverage_sites(EffectInvocationCoverage *coverage,
    uint32_t value,bool parameter,bool *complete) {
    EffectInvocationFlow *flow=coverage->flow;const EffectInvocationRead *owner=flow->read;
    if (value>=flow->values) return XR_XIR_BAD_STRUCTURE;
    const uint64_t *row=effect_invocation_row(flow,value);
    if (!row) return XR_XIR_BAD_STRUCTURE;
    bool present=false;
    for (uint32_t s=0;s<owner->final->site_count;++s) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        if (!(row[s/64]&(UINT64_C(1)<<(s%64)))) continue;
        present=true;bool found=false;
        const EffectInvocationNode *node=&owner->final->nodes[flow->node];
        for (uint32_t p=0;p<node->producer_count;++p) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            if (node->producers[p].value==value && node->producers[p].site==s && !node->producers[p].atom)
                found=true;
        }
        if (!found) { *complete=false;return XR_XIR_OK; }
    }
    if (parameter) {
        uint32_t last=owner->final->site_count+flow->basis.parameters+2;
        for (uint32_t bit=owner->final->site_count;bit<=last;++bit) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            uint32_t opaque=owner->final->site_count+flow->basis.parameters;
            if (bit!=opaque && (row[bit/64]&(UINT64_C(1)<<(bit%64)))) present=false;
        }
    }
    *complete=present;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_coverage_source(EffectInvocationCoverage *coverage,
    uint32_t value,uint64_t *maximum,bool *complete) {
    if (value>=coverage->flow->values) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(coverage->flow->read->work,2)) return XR_XIR_BUDGET;
    uint64_t rank=coverage->ranks[value];
    if (!rank) *complete=false;
    else if (rank>*maximum) *maximum=rank;
    return XR_XIR_OK;
}

/* Full-key replay selects one current edge per actual site. All targets must
 * cover every return exit; an unused known site cannot repair an unbound call. */
static XrXirStatus effect_invocation_coverage_call(EffectInvocationCoverage *coverage,
    uint32_t i,uint64_t *maximum,bool *complete) {
    EffectInvocationFlow *flow=coverage->flow;const EffectInvocationRead *owner=flow->read;
    XrXirInstruction body_op_3;const XrXirInstruction *value=NULL;
    XrXirStatus body_read_3=effect_invocation_flow_instruction(flow,i,&body_op_3,&value);
    if (body_read_3!=XR_XIR_OK) return body_read_3;uint32_t frontier=i;
    if (value->op==XR_XIR_INVOKE_RESULT) {
        if (value->immediate<0 || (uint64_t)value->immediate>=flow->function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        frontier=(uint32_t)value->immediate;
    }
    XrXirInstruction body_op_4;const XrXirInstruction *op=NULL;
    XrXirStatus body_read_4=effect_invocation_flow_instruction(flow,frontier,&body_op_4,&op);
    if (body_read_4!=XR_XIR_OK) return body_read_4;
    bool indirect=op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT;
    if (!indirect && op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE &&
        op->op!=XR_XIR_CALL_DEFAULT && op->op!=XR_XIR_INVOKE_DEFAULT) return XR_XIR_BAD_STRUCTURE;
    if (indirect) {
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->values) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=effect_invocation_coverage_source(coverage,(uint32_t)op->immediate,maximum,complete);
        if (status!=XR_XIR_OK || !*complete) return status;
    }
    if (!xir_compile_work(owner->work,(uint64_t)owner->final->site_count*sizeof(*coverage->matches)))
        return XR_XIR_BUDGET;
    memset(coverage->matches,0,(size_t)owner->final->site_count*sizeof(*coverage->matches));
    uint32_t count=0,walked=0;
    for (uint32_t e=owner->final->nodes[flow->node].edge_head;e!=UINT32_MAX;e=owner->final->edges[e].next) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        if (e>=owner->final->edge_count || ++walked>owner->final->edge_count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationEdge edge=owner->final->edges[e];
        if (edge.latent || edge.instruction!=frontier) continue;
        bool matched=false;XrXirStatus status=effect_invocation_coverage_input(flow,e,&matched);
        if (status!=XR_XIR_OK) return status;
        if (!matched) continue;
        ++count;
        if (indirect) {
            if (edge.producer>=owner->final->site_count || ++coverage->matches[edge.producer]!=1)
                return XR_XIR_BAD_STRUCTURE;
        } else if (count!=1) return XR_XIR_BAD_STRUCTURE;
        uint64_t rank=owner->final->nodes[edge.target].return_coverage;
        if (!rank) *complete=false;
        else if (rank>*maximum) *maximum=rank;
    }
    if (!count) *complete=false;
    if (indirect) {
        const uint64_t *row=effect_invocation_row(flow,(uint32_t)op->immediate);
        if (!row) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t s=0;s<owner->final->site_count;++s) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            if ((row[s/64]&(UINT64_C(1)<<(s%64))) && coverage->matches[s]!=1) *complete=false;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_coverage_value(EffectInvocationCoverage *coverage,
    uint32_t v,uint64_t *output) {
    EffectInvocationFlow *flow=coverage->flow;*output=0;
    if (v>=flow->values) return XR_XIR_BAD_STRUCTURE;
    XrXirType actual_type=0;
    XrXirStatus read_status=effect_invocation_flow_type(flow,v,&actual_type);
    if (read_status!=XR_XIR_OK) return read_status;
    if (!xr_xir_callable_signature(flow->body.domain.types,actual_type)) return XR_XIR_OK;
    bool fixed=false;read_status=effect_invocation_flag_get(flow,v,false,&fixed);
    if (read_status!=XR_XIR_OK || fixed) return read_status;
    bool complete=false;XrXirStatus status=effect_invocation_coverage_sites(coverage,v,
        v<flow->function->parameter_count,&complete);
    if (status!=XR_XIR_OK || !complete) return status;
    uint64_t maximum=0;
    if (v<flow->function->parameter_count) {
        const EffectInvocationNode *node=&flow->read->final->nodes[flow->node];
        if (!node->parameter_coverage || !node->parameter_coverage[v]) return XR_XIR_OK;
        maximum=node->parameter_coverage[v]-1;
    } else {
        uint32_t i=v-flow->function->parameter_count;
        XrXirInstruction body_op_5;const XrXirInstruction *op=NULL;
        XrXirStatus body_read_5=effect_invocation_flow_instruction(flow,i,&body_op_5,&op);
        if (body_read_5!=XR_XIR_OK) return body_read_5;
        switch (op->op) {
        case XR_XIR_FUNCTION_REF: {
            uint32_t site=UINT32_MAX;bool supported=false;
            status=effect_invocation_site(flow,i,&site);
            if (status==XR_XIR_OK) status=effect_invocation_producer_site_supported_read(flow->read,&flow->body,site,&supported);
            bool certified=false;
            if (status==XR_XIR_OK) status=effect_invocation_flag_get(flow,v,true,&certified);
            complete=certified && supported;break;
        }
        case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
        case XR_XIR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_NEW:
        case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_READ:
            status=effect_invocation_coverage_source(coverage,op->args[0],&maximum,&complete);break;
        case XR_XIR_PHI:
            if (!op->args[1] || op->args[1]%2 || !flow->function->operands ||
                op->args[0]>flow->function->operand_count ||
                op->args[1]>flow->function->operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
            for (uint32_t a=1;status==XR_XIR_OK && a<op->args[1];a+=2)
                status=effect_invocation_coverage_source(coverage,
                    flow->function->operands[op->args[0]+a],&maximum,&complete);
            break;
        case XR_XIR_CALL: case XR_XIR_CALL_DEFAULT: case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_RESULT:
            status=effect_invocation_coverage_call(coverage,i,&maximum,&complete);break;
        default:complete=false;break;
        }
    }
    uint32_t walked=0;
    for (uint32_t i=coverage->writes[v];status==XR_XIR_OK && i!=UINT32_MAX;i=coverage->next[i]) {
        if (i>=flow->function->instruction_count || ++walked>flow->function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        XrXirInstruction write_storage;const XrXirInstruction *write=NULL;
        status=effect_invocation_flow_instruction(flow,i,&write_storage,&write);
        if (status==XR_XIR_OK)
            status=effect_invocation_coverage_source(coverage,write->args[1],&maximum,&complete);
    }
    if (status==XR_XIR_OK && complete) {
        if (maximum==UINT64_MAX) return XR_XIR_BUDGET;
        *output=maximum+1;
    }
    return status;
}

static XrXirStatus effect_invocation_coverage_values(EffectInvocationFlow *flow,
    EffectInvocationCoverage *coverage) {
    XrXirStatus status=effect_invocation_coverage_storage(flow,coverage);
    bool changed=true;
    while (status==XR_XIR_OK && changed) {
        changed=false;
        for (uint32_t v=0;v<flow->values;++v) {
            if (!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
            if (coverage->ranks[v]) continue;
            uint64_t rank=0;status=effect_invocation_coverage_value(coverage,v,&rank);
            if (status!=XR_XIR_OK) break;
            if (rank) { coverage->ranks[v]=rank;changed=true; }
        }
    }
    return status;
}

#include "xxir_effect_invocation_parameter_coverage.inc.c"

/* Every actual RETURN is checked, including conservatively unreachable
 * blocks. A throw-only or cyclic bottom cannot manufacture a returned target. */
static XrXirStatus effect_invocation_coverage_return_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,uint32_t n,uint64_t *output) {
    *output=0;bool supported=false;
    XrXirStatus status=effect_invocation_producer_return_read(owner,request,owner->final->nodes[n].body,&supported);
    if (status!=XR_XIR_OK || !supported) return status;
    EffectInvocationFlow flow={.read=owner,.node=n,.instruction=UINT32_MAX};
    status=effect_invocation_origins_read(&flow,request);EffectInvocationCoverage coverage={0};
    if (status==XR_XIR_OK) status=effect_invocation_coverage_values(&flow,&coverage);
    EffectBodyRecordCursor cursor;
    if (status==XR_XIR_OK) status=effect_body_request_sequence_begin(&flow.body,&cursor);
    bool present=false,complete=true;uint64_t maximum=0;
    for (uint32_t i=0;status==XR_XIR_OK && i<flow.function->instruction_count;++i) {
        if (!xir_compile_work(owner->work,1)) { status=XR_XIR_BUDGET;break; }
        XrXirInstruction body_op_6;const XrXirInstruction *op=NULL;
        status=effect_body_request_sequence_instruction(&flow.body,
            owner->final->nodes[n].body,i,&cursor,&body_op_6,&op);
        if (status!=XR_XIR_OK) break;
        if (op->op!=XR_XIR_RETURN) continue;
        present=true;status=effect_invocation_coverage_source(&coverage,op->args[0],&maximum,&complete);
    }
    if (status==XR_XIR_OK)
        status=effect_body_request_sequence_finish(&flow.body,owner->final->nodes[n].body,&cursor);
    if (status==XR_XIR_OK && present && complete) {
        if (maximum==UINT64_MAX) status=XR_XIR_BUDGET;
        else *output=maximum+1;
    }
    xr_compile_resources_free(coverage.memory);effect_invocation_flow_finish(&flow);return status;
}
static XrXirStatus effect_invocation_coverage_return(EffectInvocationWorkspace *owner,
    uint32_t n,uint64_t *output) {
    if (!owner || !effect_body_material_shape(owner->final.material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(owner->read.work,owner->final.material);
    return effect_invocation_coverage_return_read(&owner->read,&request,n,output);
}

/* Target discovery has already reached closure. Positive coverage uses the
 * same owned graph queue and cannot add nodes, edges, ROOT bits or permissions. */
static XrXirStatus effect_invocation_coverage_solve(EffectInvocationWorkspace *owner) {
    if (!owner || owner->pending || !owner->graph || !owner->read.work || !owner->states)
        return XR_XIR_BAD_STRUCTURE;
    if (!owner->read.producer_enabled) return XR_XIR_OK;
    XrXirStatus shape=effect_invocation_coverage_incoming_shape(&owner->read);
    if (shape!=XR_XIR_OK) return shape;
    for (uint32_t n=0;n<owner->final.count;++n) {
        if (owner->final.nodes[n].return_coverage || owner->states[n].queued ||
            (!!owner->final.nodes[n].parameter_coverage!=!!owner->final.nodes[n].parameter_count))
            return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p=0;p<owner->final.nodes[n].parameter_count;++p) {
            if (!xir_compile_work(owner->read.work,1)) return XR_XIR_BUDGET;
            if (owner->final.nodes[n].parameter_coverage[p]) return XR_XIR_BAD_STRUCTURE;
        }
        XrXirStatus status=effect_invocation_enqueue(owner,n);
        if (status!=XR_XIR_OK) return status;
    }
    while (owner->pending) {
        if (!xir_compile_work(owner->read.work,3)) return XR_XIR_BUDGET;
        uint32_t n=owner->graph->queue[owner->front];
        if (n>=owner->final.count || !owner->states[n].queued) return XR_XIR_BAD_STRUCTURE;
        owner->front=owner->front+1==owner->final.capacity?0:owner->front+1;
        --owner->pending;owner->states[n].queued=false;
        bool changed=false;XrXirStatus status=effect_invocation_parameter_step(owner,n,&changed,false,0);
        uint64_t rank=0;
        if (status==XR_XIR_OK && !owner->final.nodes[n].return_coverage)
            status=effect_invocation_coverage_return(owner,n,&rank);
        if (status!=XR_XIR_OK) return status;
        if (rank) { owner->final.nodes[n].return_coverage=rank;changed=true; }
        if (changed) {
            status=effect_invocation_coverage_notify(owner,n);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

/* Strict rank descent makes forged circular completeness fail independently
 * of queue history. Exact stable input replay proves every positive clause. */
static XrXirStatus effect_invocation_coverage_verify_read(const EffectInvocationRead *owner,const EffectBodyRequest *request) {
    if (!owner || !owner->work || !owner->final->roots || !request ||
        !request->owner || !request->domain.types || !request->domain.function_count ||
        !owner->final->nodes || !owner->final->count) return XR_XIR_BAD_STRUCTURE;
    if (!owner->producer_enabled) {
        /* A disabled producer has no positive coverage authority. Validate
         * its real storage without running the producer replay algorithm. */
        for (uint32_t n=0;n<owner->final->count;++n) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            XrXirStatus zero=effect_invocation_parameter_zero_shape(owner,request,n);
            if (zero!=XR_XIR_OK) return zero;
            if (owner->final->nodes[n].return_coverage) return XR_XIR_BAD_STRUCTURE;
        }
        return XR_XIR_OK;
    }
    XrXirStatus shape=effect_invocation_coverage_incoming_shape(owner);
    if (shape!=XR_XIR_OK) return shape;
    uint64_t maximum=0;
    for (uint32_t n=0;n<owner->final->count;++n) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (owner->final->nodes[n].body>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus zero=effect_invocation_parameter_zero_shape(owner,request,n);
        if (zero!=XR_XIR_OK) return zero;
        EffectBodyFunctionHead head={0};
        XrXirStatus head_status=effect_body_request_head(request,owner->final->nodes[n].body,&head);
        if (head_status!=XR_XIR_OK) return head_status;
        const EffectBodyFunctionHead *function=&head;
        uint64_t vertices=(uint64_t)function->parameter_count+function->instruction_count+1;
        if (vertices>UINT64_MAX-maximum) return XR_XIR_BUDGET;
        maximum+=vertices;
    }
    for (uint32_t n=0;n<owner->final->count;++n) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        uint64_t stored=owner->final->nodes[n].return_coverage;
        if (stored>maximum) return XR_XIR_BAD_STRUCTURE;
        bool changed=false;XrXirStatus status=effect_invocation_parameter_step_read(owner,request,n,&changed,NULL,maximum);
        uint64_t actual=0;
        if (status==XR_XIR_OK) status=effect_invocation_coverage_return_read(owner,request,n,&actual);
        if (status!=XR_XIR_OK) return status;
        if (!!stored!=!!actual || (stored && actual>stored)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_invocation_coverage_verify(EffectInvocationWorkspace *owner) {
    if (!owner || owner->pending || !effect_body_material_shape(owner->final.material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(owner->read.work,owner->final.material);
    return effect_invocation_coverage_verify_read(&owner->read,&request);
}
