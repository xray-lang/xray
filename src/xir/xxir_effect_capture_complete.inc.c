/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_capture_complete.inc.c - Universal callable source evidence
 *
 * KEY CONCEPT:
 *   Complete sources use the same finite relation as origins. Each actual
 *   alternative is an obligation, and every dependency has a smaller rank.
 *   The receiving walk neither allocates nor reconstructs a flow solution.
 */
typedef struct EffectCaptureComplete {
    uint64_t rank;
    bool complete,admitted;
} EffectCaptureComplete;

static XrXirStatus effect_capture_complete_source(const EffectCaptureRead *read,
    uint32_t node,uint32_t value,uint32_t column,EffectCaptureComplete *result) {
    const EffectCaptureFact *fact=NULL;
    XrXirStatus status=effect_capture_find(read,
        (EffectCaptureFact){node,value,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_COMPLETE+column,0},&fact);
    if(status!=XR_XIR_OK) return status;
    if(!fact) result->complete=false;
    else if(fact->rank>=result->rank) result->rank=fact->rank+1;
    return XR_XIR_OK;
}

/* A zero column names the value itself. Other columns name the unique actual
 * captured formal, even when its current descriptor contains no origin bits.
 * Empty columns need source evidence too: an unknown callback may hide a
 * nested environment and cannot prove absence by an empty physical row. */
static XrXirStatus effect_capture_complete_column(const EffectCaptureRead *read,uint32_t column,bool *callable) {
    *callable=false;if(!column || column>read->final->binding_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t binding=column-1;
    for(uint32_t s=0;s<read->final->site_count;++s) {
        if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
        const EffectInvocationSite *site=&read->final->sites[s];
        if(binding<site->binding || binding-site->binding>=site->captures) continue;
        EffectBodyFunctionHead target={0};XrXirStatus status=effect_body_request_head(&read->body,site->target,&target);
        if(status!=XR_XIR_OK) return status;
        uint32_t parameter=binding-site->binding;
        if(parameter>=target.parameter_count) return XR_XIR_BAD_STRUCTURE;
        *callable=effect_physical_origin(read->body.domain.types,target.parameters[parameter]);
        return XR_XIR_OK;
    }
    return XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_capture_complete_admit(const EffectCaptureRead *read,
    const EffectCaptureNode *node,uint32_t value,uint32_t column,bool *output) {
    *output=false;uint32_t kind=0;
    XrXirStatus status=effect_capture_kind(read,node,value,&kind,NULL);
    if(status!=XR_XIR_OK || kind!=EFFECT_CAPTURE_CALLABLE) return status;
    if(value<node->values) {
        const EffectCaptureFact *fixed=NULL;
        status=effect_capture_find(read,(EffectCaptureFact){node->node,value,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_FIXED,0},&fixed);
        if(status!=XR_XIR_OK || fixed) return status;
    }
    if(column) return effect_capture_complete_column(read,column,output);
    EffectCaptureFact key={node->node,value,0,0,0};uint32_t at=0;
    status=effect_capture_lower(read,&key,&at);if(status!=XR_XIR_OK) return status;
    bool present=false;
    for(;at<read->final->capture.count;++at) {
        if(!xir_compile_work(read->work,4)) return XR_XIR_BUDGET;
        const EffectCaptureFact *fact=&read->final->capture.facts[at];
        if(fact->node!=node->node || fact->value!=value || fact->word>=node->basis.origin_words) break;
        uint64_t bit=(uint64_t)fact->word*64+fact->bit;
        if(bit<read->final->site_count) present=true;
        else if((value<node->head.parameter_count || value>node->values) &&
            bit!=read->final->site_count+node->basis.parameters) return XR_XIR_OK;
    }
    *output=present;return XR_XIR_OK;
}

static XrXirStatus effect_capture_complete_initial(const EffectCaptureRead *read,
    const EffectCaptureNode *node,uint32_t parameter,uint32_t column,EffectCaptureComplete *result) {
    if(read->final->roots[read->final->nodes[node->node].body]==node->node) {
        result->complete=false;result->admitted=false;return XR_XIR_OK;
    }
    uint32_t count=0;
    for(uint32_t e=0;e<read->final->edge_count;++e) {
        if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
        if(read->final->edges[e].target!=node->node) continue;
        bool matched=false;XrXirStatus status=effect_capture_edge_complete(read,e,&matched);
        if(status!=XR_XIR_OK) return status;
        if(!matched) continue;
        ++count;EffectCaptureActual actual={0};status=effect_capture_actual(read,e,parameter,&actual);
        if(status!=XR_XIR_OK) return status;
        if(actual.unbound) result->complete=false;
        else if(actual.captured) {
            const EffectInvocationEdge *edge=&read->final->edges[e];
            if(edge->producer>=read->final->site_count) return XR_XIR_BAD_STRUCTURE;
            const EffectInvocationSite *site=&read->final->sites[edge->producer];
            if(parameter>=site->captures || (uint64_t)site->binding+parameter+1>UINT32_MAX-EFFECT_CAPTURE_COMPLETE)
                return XR_XIR_BAD_STRUCTURE;
            uint32_t captured=site->binding+parameter+1;
            status=effect_capture_complete_source(read,actual.node,actual.value,0,result);
            if(status==XR_XIR_OK && result->complete)
                status=effect_capture_complete_source(read,actual.node,actual.value,captured,result);
            if(status==XR_XIR_OK && result->complete && column)
                status=effect_capture_complete_source(read,actual.node,actual.value,column,result);
        } else status=effect_capture_complete_source(read,actual.node,actual.value,column,result);
        if(status!=XR_XIR_OK || !result->complete) return status;
    }
    if(!count) result->complete=false;
    return XR_XIR_OK;
}

static XrXirStatus effect_capture_complete_call(const EffectCaptureRead *read,
    const EffectCaptureNode *node,uint32_t instruction,uint32_t column,EffectCaptureComplete *result) {
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    uint32_t body=read->final->nodes[node->node].body,frontier=instruction;
    XrXirStatus status=effect_body_request_instruction(&read->body,body,frontier,&storage,&op);
    if(status!=XR_XIR_OK) return status;
    if(op->op==XR_XIR_INVOKE_RESULT) {
        if(op->immediate<0 || (uint64_t)op->immediate>=node->head.instruction_count) return XR_XIR_BAD_STRUCTURE;
        frontier=(uint32_t)op->immediate;
        status=effect_body_request_instruction(&read->body,body,frontier,&storage,&op);
        if(status!=XR_XIR_OK) return status;
    }
    bool indirect=op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT;
    if(!indirect && op->op!=XR_XIR_CALL && op->op!=XR_XIR_CALL_DEFAULT &&
        op->op!=XR_XIR_INVOKE && op->op!=XR_XIR_INVOKE_DEFAULT) return XR_XIR_BAD_STRUCTURE;
    uint32_t callee=0;
    if(indirect) {
        if(op->immediate<0 || (uint64_t)op->immediate>=node->values) return XR_XIR_BAD_STRUCTURE;
        callee=(uint32_t)op->immediate;
        status=effect_capture_complete_source(read,node->node,callee,0,result);
        if(status!=XR_XIR_OK || !result->complete) return status;
    }
    uint32_t count=0;
    for(uint32_t e=0;e<read->final->edge_count;++e) {
        if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
        const EffectInvocationEdge *edge=&read->final->edges[e];
        if(edge->caller!=node->node || edge->instruction!=frontier || edge->latent) continue;
        bool matched=false;status=effect_capture_edge_complete(read,e,&matched);
        if(status!=XR_XIR_OK) return status;
        if(!matched) continue;
        ++count;EffectCaptureNode target={0};status=effect_capture_node(read,edge->target,&target);
        if(status!=XR_XIR_OK) return status;
        const XrXirTypeNode *signature=xr_xir_callable_signature(read->body.domain.types,target.head.result);
        bool returned=effect_physical_iterator(read->body.domain.types,target.head.result) || (signature && !signature->parameter_span &&
            (signature->flags&XR_XIR_CALLABLE_ROOT_MASK)==XR_XIR_CALLABLE_ROOT_NONE);
        for(uint32_t p=0;signature && p<signature->parameter_count;++p) {
            if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
            if(!xr_xir_callable_parameter_storage_valid(read->body.domain.types,&signature->parameters[p])) return XR_XIR_BAD_TYPE;
            if(signature->parameters[p].mode!=XR_PARAM_READ) returned=false;
        }
        if(!returned) { result->complete=false;return XR_XIR_OK; }
        status=effect_capture_complete_source(read,edge->target,target.values,column,result);
        if(status!=XR_XIR_OK || !result->complete) return status;
    }
    if(!count || (!indirect && count!=1)) { result->complete=false;return XR_XIR_OK; }
    if(!indirect) return XR_XIR_OK;
    for(uint32_t s=0;s<read->final->site_count;++s) {
        const EffectCaptureFact *origin=NULL;
        status=effect_capture_find(read,(EffectCaptureFact){node->node,callee,s/64,s%64,0},&origin);
        if(status!=XR_XIR_OK) return status;
        if(!origin) continue;
        uint32_t matches=0;
        for(uint32_t e=0;e<read->final->edge_count;++e) {
            if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
            const EffectInvocationEdge *edge=&read->final->edges[e];
            if(edge->caller!=node->node || edge->instruction!=frontier || edge->latent || edge->producer!=s) continue;
            bool matched=false;status=effect_capture_edge_complete(read,e,&matched);
            if(status!=XR_XIR_OK) return status;
            if(matched) ++matches;
        }
        if(matches!=1) { result->complete=false;return XR_XIR_OK; }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_capture_complete_selected(const EffectCaptureRead *read,
    const EffectCaptureNode *node,uint32_t value,uint32_t selection,uint32_t column,EffectCaptureComplete *result) {
    uint32_t count=0;EffectFinalInputSelect cursor;
    XrXirStatus status=effect_final_input_select_begin(&read->body,selection,&cursor);
    if(status!=XR_XIR_OK) return status;
    for(;;) {
        XrXirInstruction op;uint32_t i=0;bool found=false;
        status=effect_final_input_select_next(&read->body,read->final->nodes[node->node].body,&cursor,&i,&op,&found);
        if(status!=XR_XIR_OK || !found) break;
        if(selection==EFFECT_FINAL_SELECT_WRITE && op.args[0]!=value) continue;
        ++count;status=effect_capture_complete_source(read,node->node,
            op.args[selection==EFFECT_FINAL_SELECT_WRITE?1:0],column,result);
        if(status!=XR_XIR_OK || !result->complete) return status;
    }
    if(selection==EFFECT_FINAL_SELECT_RETURN && !count) result->complete=false;
    return status;
}

static XrXirStatus effect_capture_complete_reference(const EffectCaptureRead *read,
    const EffectCaptureNode *node,uint32_t instruction,uint32_t column,EffectCaptureComplete *result) {
    result->complete=false;
    for(uint32_t s=0;s<read->final->site_count;++s) {
        if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
        const EffectInvocationSite *site=&read->final->sites[s];
        if(site->function!=read->final->nodes[node->node].body || site->instruction!=instruction) continue;
        XrXirStatus status=effect_capture_reference_seed(read,s,&result->complete);
        if(status!=XR_XIR_OK || !result->complete || !column) return status;
        EffectBodyFunctionHead target={0};XrXirInstruction storage;const XrXirInstruction *op=NULL;
        status=effect_body_request_head(&read->body,site->target,&target);
        if(status==XR_XIR_OK) status=effect_body_request_instruction(&read->body,site->function,instruction,&storage,&op);
        if(status!=XR_XIR_OK) return status;
        if(op->args[0]>node->head.operand_count || site->captures>node->head.operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
        for(uint32_t p=0;p<site->captures;++p) {
            if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
            if(!effect_physical_origin(read->body.domain.types,target.parameters[p])) continue;
            uint32_t source=node->head.operands[op->args[0]+p];
            if(column-1==site->binding+p) {
                status=effect_capture_complete_source(read,node->node,source,0,result);
                if(status!=XR_XIR_OK || !result->complete) return status;
            }
            status=effect_capture_complete_source(read,node->node,source,column,result);
            if(status!=XR_XIR_OK || !result->complete) return status;
        }
        return XR_XIR_OK;
    }
    return XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_capture_complete_value(const EffectCaptureRead *read,
    const EffectCaptureNode *node,uint32_t value,uint32_t column,EffectCaptureComplete *result) {
    *result=(EffectCaptureComplete){.complete=true};bool admitted=false;
    XrXirStatus status=effect_capture_complete_admit(read,node,value,column,&admitted);
    if(status!=XR_XIR_OK || !admitted) { result->complete=false;return status; }
    result->admitted=true;
    if(value>node->values) return effect_capture_complete_initial(read,node,value-node->values-1,column,result);
    if(value==node->values) return effect_capture_complete_selected(read,node,value,EFFECT_FINAL_SELECT_RETURN,column,result);
    if(value<node->head.parameter_count) {
        status=effect_capture_complete_source(read,node->node,node->values+1+value,column,result);
    } else {
        uint32_t i=value-node->head.parameter_count;XrXirInstruction storage;const XrXirInstruction *op=NULL;
        status=effect_body_request_instruction(&read->body,read->final->nodes[node->node].body,i,&storage,&op);
        if(status!=XR_XIR_OK) return status;
        switch(op->op) {
        case XR_XIR_FUNCTION_REF:case XR_XIR_GENERATOR_NEW:status=effect_capture_complete_reference(read,node,i,column,result);break;
        case XR_XIR_COPY:case XR_XIR_SCALAR_COPY:case XR_XIR_OWNED_RETAIN:
        case XR_XIR_LOCAL_NEW:case XR_XIR_SCALAR_LOCAL_NEW:case XR_XIR_OWNED_LOCAL_NEW:
        case XR_XIR_LOCAL_READ:case XR_XIR_SCALAR_LOCAL_READ:case XR_XIR_OWNED_LOCAL_READ:
            status=effect_capture_complete_source(read,node->node,op->args[0],column,result);break;
        case XR_XIR_PHI:
            if(!op->args[1] || op->args[1]%2 || op->args[0]>node->head.operand_count ||
                op->args[1]>node->head.operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
            for(uint32_t p=1;p<op->args[1];p+=2) {
                status=effect_capture_complete_source(read,node->node,node->head.operands[op->args[0]+p],column,result);
                if(status!=XR_XIR_OK || !result->complete) return status;
            }
            break;
        case XR_XIR_CALL:case XR_XIR_CALL_DEFAULT:case XR_XIR_CALL_INDIRECT:case XR_XIR_INVOKE_RESULT:
            status=effect_capture_complete_call(read,node,i,column,result);break;
        default:result->complete=false;result->admitted=false;break;
        }
    }
    if(status!=XR_XIR_OK || !result->complete) return status;
    return effect_capture_complete_selected(read,node,value,EFFECT_FINAL_SELECT_WRITE,column,result);
}

typedef struct EffectCaptureCompleteCoordinate {
    uint32_t node,value,column;
} EffectCaptureCompleteCoordinate;
typedef struct EffectCaptureCompleteJobs {
    EffectCaptureCompleteCoordinate values[128];
    uint32_t count;
    bool overflow;
} EffectCaptureCompleteJobs;

static XrXirStatus effect_capture_complete_open(EffectCaptureRead *read,
    EffectBodyCurrentRead *current,EffectCaptureNode *views,EffectCaptureActualRead *actuals) {
    if(!read->builder || read->body.current) return XR_XIR_OK;
    *current=(EffectBodyCurrentRead){0};
    XrXirStatus status=effect_body_current_open(read->work,read->final->material,current);
    if(status!=XR_XIR_OK) return status;
    read->body.current=current;
    if(views && actuals) {
        if(read->final->count<=32) {
            for(uint32_t at=0;at<read->final->count;++at) {
                status=effect_capture_node(read,at,&views[at]);if(status!=XR_XIR_OK) return status;
            }
            read->node_views=views;read->node_view_count=read->final->count;
        }
        status=effect_capture_actuals_read(read,actuals);if(status!=XR_XIR_OK) return status;
        if(actuals->edges) read->actuals=actuals;
    }
    return XR_XIR_OK;
}

/* Every source loan ends before a growing emit. In-place Complete writes
 * preserve the current body and origin projections used by the next job. */
static XrXirStatus effect_capture_complete_put(EffectCaptureRead *read,
    EffectBodyCurrentRead *current,EffectCaptureNode *borrowed,
    EffectCaptureCompleteCoordinate coordinate,const EffectCaptureComplete *result) {
    EffectCaptureFact key={coordinate.node,coordinate.value,EFFECT_CAPTURE_FLAGS,
        EFFECT_CAPTURE_COMPLETE+coordinate.column,result->rank};
    const EffectCaptureFact *stored=NULL;XrXirStatus status=effect_capture_find(read,key,&stored);
    if(status!=XR_XIR_OK) return status;
    if(!read->builder && stored && (!result->complete || stored->rank!=result->rank)) return XR_XIR_BAD_STRUCTURE;
    if(!result->complete) return XR_XIR_OK;
    bool growing=read->builder && read->builder->count==read->builder->capacity && !stored;
    if(growing) {
        read->node_view=NULL;read->body.current=NULL;
        read->node_views=NULL;read->node_view_count=0;read->actuals=NULL;
        *borrowed=(EffectCaptureNode){0};*current=(EffectBodyCurrentRead){0};
    }
    status=effect_capture_emit(read,key);if(status!=XR_XIR_OK) return status;
    return growing?effect_capture_frame(read):XR_XIR_OK;
}

static XrXirStatus effect_capture_complete_walk(EffectCaptureRead *read,
    EffectBodyCurrentRead *current,EffectCaptureNode *borrowed,
    EffectCaptureNode *views,EffectCaptureActualRead *actuals,EffectCaptureCompleteJobs *jobs) {
    if(read->final->binding_count>UINT32_MAX-EFFECT_CAPTURE_COMPLETE) return XR_XIR_BAD_STRUCTURE;
    for(uint32_t n=0;n<read->final->count;++n) for(uint32_t value=0;;++value) {
        uint32_t column=0;bool present=false;
        for(;;) {
            XrXirStatus status=effect_capture_complete_open(read,current,views,actuals);
            if(status!=XR_XIR_OK) return status;
            /* Heads remain borrowed only until an emit can invoke allocation
             * or release. No borrowed field survives that callback boundary. */
            EffectCaptureComplete result={0};
            {
                if(!read->node_view || read->node_view->node!=n) {
                    read->node_view=NULL;status=effect_capture_node(read,n,borrowed);
                    if(status!=XR_XIR_OK) return status;
                    read->node_view=borrowed;
                }
                const EffectCaptureNode *node=NULL;
                status=effect_capture_node_borrow(read,n,borrowed,&node);
                if(status!=XR_XIR_OK) return status;
                uint64_t end=(uint64_t)node->values+1+node->head.parameter_count;
                if(value>=end) break;
                present=true;
                if(!column) {
                    uint32_t kind=0;status=effect_capture_kind(read,node,value,&kind,NULL);
                    if(status!=XR_XIR_OK) return status;
                    if(kind!=EFFECT_CAPTURE_CALLABLE) {
                        /* No Complete column belongs to a non-callable value.
                         * One exact range absence proves every column, using
                         * the current type and all possible stored flags. */
                        EffectCaptureFact key={n,value,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_COMPLETE,0};uint32_t at=0;
                        status=effect_capture_lower(read,&key,&at);if(status!=XR_XIR_OK) return status;
                        if(at<read->final->capture.count) {
                            if(!xir_compile_work(read->work,4)) return XR_XIR_BUDGET;
                            const EffectCaptureFact *fact=&read->final->capture.facts[at];
                            if(fact->node==n && fact->value==value && fact->word==EFFECT_CAPTURE_FLAGS &&
                                fact->bit>=EFFECT_CAPTURE_COMPLETE) return XR_XIR_BAD_STRUCTURE;
                        }
                        break;
                    }
                }
                status=effect_capture_complete_value(read,node,value,column,&result);if(status!=XR_XIR_OK) return status;
            }
            EffectCaptureCompleteCoordinate coordinate={n,value,column};
            if(jobs && result.admitted && !jobs->overflow) {
                if(jobs->count==XR_COUNTOF(jobs->values)) {
                    if(!xir_compile_work(read->work,sizeof(jobs->overflow))) return XR_XIR_BUDGET;
                    jobs->overflow=true;
                }
                else {
                    if(!xir_compile_work(read->work,sizeof(coordinate)+sizeof(jobs->count))) return XR_XIR_BUDGET;
                    jobs->values[jobs->count++]=coordinate;
                }
            }
            status=effect_capture_complete_put(read,current,borrowed,coordinate,&result);
            if(status!=XR_XIR_OK) return status;
            if(column==read->final->binding_count) break;
            ++column;
        }
        if(!present) break;
        if(value==UINT32_MAX) return XR_XIR_BUDGET;
    }
    return XR_XIR_OK;
}

/* Admission depends on the fixed origin/type domain, not on Complete facts.
 * Ordinals collected by the first full sweep select subsequent builder work;
 * every job recomputes its current rule and rank. Receiving still walks the
 * entire actual domain, independently of these constructor proposals. */
static XrXirStatus effect_capture_complete_jobs(EffectCaptureRead *read,
    EffectBodyCurrentRead *current,EffectCaptureNode *borrowed,
    EffectCaptureNode *views,EffectCaptureActualRead *actuals,const EffectCaptureCompleteJobs *jobs) {
    if(!read->builder || !read->complete_scope || !jobs || jobs->overflow) return XR_XIR_BAD_STRUCTURE;
    for(uint32_t j=0;j<jobs->count;++j) {
        if(!xir_compile_work(read->work,sizeof(EffectCaptureCompleteCoordinate))) return XR_XIR_BUDGET;
        EffectCaptureCompleteCoordinate coordinate=jobs->values[j];
        XrXirStatus status=effect_capture_complete_open(read,current,views,actuals);
        if(status!=XR_XIR_OK) return status;
        EffectCaptureComplete result={0};
        {
            const EffectCaptureNode *node=NULL;
            status=effect_capture_node_borrow(read,coordinate.node,borrowed,&node);
            if(status!=XR_XIR_OK) return status;
            status=effect_capture_complete_value(read,node,coordinate.value,coordinate.column,&result);
        }
        if(status==XR_XIR_OK) status=effect_capture_complete_put(read,current,borrowed,coordinate,&result);
        if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* The view contains current Input identity only. In-place fact writes have no
 * external callback; a possible growth discards both views before emitting and
 * frames the actual Input and Final again afterwards. The caller receives no
 * retained stack address, on either success or failure. */
static XrXirStatus effect_capture_complete_empty(const EffectCaptureRead *read,bool *empty) {
    *empty=false;
    if(read->final->site_count) return XR_XIR_OK;
    if(read->final->binding_count) return XR_XIR_BAD_STRUCTURE;
    /* The current framed site inventory is empty. Admission requires a real
     * finite origin bit, so neither a value nor any capture column can be
     * Complete. Check every stored claim instead of walking empty columns. */
    for(uint32_t p=0;p<read->final->capture.count;++p) {
        if(!xir_compile_work(read->work,4)) return XR_XIR_BUDGET;
        const EffectCaptureFact *fact=&read->final->capture.facts[p];
        if(fact->word==EFFECT_CAPTURE_FLAGS && fact->bit>=EFFECT_CAPTURE_COMPLETE)
            return XR_XIR_BAD_STRUCTURE;
    }
    *empty=true;return XR_XIR_OK;
}
static XrXirStatus effect_capture_complete_clauses(EffectCaptureRead *read) {
    bool empty=false;XrXirStatus status=effect_capture_complete_empty(read,&empty);
    if(status!=XR_XIR_OK || empty) return status;
    const EffectBodyCurrentRead *saved_current=read->body.current;
    const EffectCaptureNode *saved_node=read->node_view;
    if(read->builder && (saved_current || saved_node)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyCurrentRead current={0};EffectCaptureNode borrowed={0};
    read->node_view=NULL;
    status=effect_capture_complete_walk(read,&current,&borrowed,NULL,NULL,NULL);
    read->body.current=saved_current;read->node_view=saved_node;
    return status;
}

/* Only Complete flags change during this construction interval. Source
 * descriptors therefore remain current until a growing emit invokes a
 * callback; the walk drops every projection first and rebuilds afterwards. */
static XrXirStatus effect_capture_complete_construct(EffectCaptureRead *read) {
    if(!read->builder || read->complete_scope || read->body.current || read->node_view || read->node_views || read->actuals)
        return XR_XIR_BAD_STRUCTURE;
    bool empty=false;XrXirStatus status=effect_capture_complete_empty(read,&empty);
    if(status!=XR_XIR_OK || empty) return status;
    EffectBodyCurrentRead current={0};EffectCaptureNode borrowed={0},views[32];EffectCaptureActualRead actuals;
    EffectCaptureCompleteJobs jobs;
    if(!xir_compile_work(read->work,sizeof(jobs.count)+sizeof(jobs.overflow))) return XR_XIR_BUDGET;
    jobs.count=0;jobs.overflow=false;
    read->complete_scope=true;read->changed=false;
    status=effect_capture_complete_walk(read,&current,&borrowed,views,&actuals,&jobs);
    while(status==XR_XIR_OK && read->changed) {
        read->changed=false;
        status=jobs.overflow?effect_capture_complete_walk(read,&current,&borrowed,views,&actuals,NULL):
            effect_capture_complete_jobs(read,&current,&borrowed,views,&actuals,&jobs);
    }
    read->complete_scope=false;read->body.current=NULL;read->node_view=NULL;
    read->node_views=NULL;read->node_view_count=0;read->actuals=NULL;return status;
}
