/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_capture_edges.inc.c - Actual argument and return origin clauses
 *
 * KEY CONCEPT:
 *   Captured prefixes project the descriptor at the real call. Initial keys
 *   retain supported historical subsets; each actual frontier still requires
 *   one complete current key, including all residual and capture columns.
 */
typedef struct EffectCaptureActual {
    uint32_t node,value,first,kind,mask;
    uint64_t rank;
    bool captured,unbound,opaque,active;
} EffectCaptureActual;

static XrXirStatus effect_capture_edge_active(const EffectCaptureRead *read,uint32_t selected,
    uint64_t *rank,bool *active) {
    const EffectInvocationEdge *edge=&read->final->edges[selected];*rank=0;*active=false;
    if(edge->caller>=read->final->count || edge->target>=read->final->count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_final_edge_payload(read->work,&read->body,read->final,selected);
    if(status!=XR_XIR_OK) return status;
    if(edge->producer==UINT32_MAX) { *active=true;return XR_XIR_OK; }
    EffectCaptureNode node_storage={0};
    const EffectCaptureNode *node=NULL;
    status=effect_capture_node_borrow(read,edge->caller,&node_storage,&node);if(status!=XR_XIR_OK) return status;
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    status=effect_body_request_instruction(&read->body,read->final->nodes[edge->caller].body,edge->instruction,&storage,&op);
    if(status!=XR_XIR_OK) return status;
    uint32_t value=op->op==XR_XIR_FUNCTION_REF?node->head.parameter_count+edge->instruction:(uint32_t)op->immediate;
    const EffectCaptureFact *fact=NULL;
    status=effect_capture_find(read,(EffectCaptureFact){edge->caller,value,edge->producer/64,edge->producer%64,0},&fact);
    if(status!=XR_XIR_OK) return status;
    if(fact) { *rank=fact->rank;*active=true; }
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_argument_bound(const EffectCaptureRead *read,
    const EffectInvocationEdge *edge,uint32_t parameter,bool force,EffectCaptureActual *actual) {
    EffectCaptureNode target_storage={0};
    const EffectCaptureNode *target=NULL;
    XrXirStatus status=effect_capture_node_borrow(read,edge->target,&target_storage,&target);
    if(status!=XR_XIR_OK) return status;
    const XrXirTypeNode *signature=NULL;
    status=effect_final_edge_signature(read->work,read->body.domain.types,target->head.parameters[parameter],&signature);
    if(status!=XR_XIR_OK) return status;
    if(actual->unbound) { actual->opaque=true;actual->mask=signature->flags;return XR_XIR_OK; }
    if(!force) {
        const EffectCaptureFact *fixed=NULL;
        status=effect_capture_find(read,(EffectCaptureFact){actual->node,actual->value,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_FIXED,0},&fixed);
        if(status!=XR_XIR_OK || fixed) return status;
    }
    uint32_t role=0;status=effect_role_parameter_read(read->work,read->final->material,
        &read->final->role_proof,read->final->nodes[edge->target].body,parameter,&role);
    if(status!=XR_XIR_OK || role!=XR_XIR_EFFECT_PARAMETER_FIXED) return status;
    bool ref=false;
    for(uint32_t p=0;p<signature->parameter_count;++p) {
        if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
        if(signature->parameters[p].mode==XR_PARAM_REF) ref=true;
    }
    actual->mask=signature->flags&(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    actual->opaque=!ref || (actual->mask&XR_XIR_CALLABLE_ROOT_REQUIRED);
    if(ref) actual->mask&=XR_XIR_CALLABLE_ROOT_REQUIRED;
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_actual(const EffectCaptureRead *read,uint32_t selected,
    uint32_t parameter,EffectCaptureActual *output) {
    const EffectInvocationEdge *edge=&read->final->edges[selected];
    EffectCaptureNode caller_storage={0};
    const EffectCaptureNode *caller=NULL;
    EffectCaptureNode target_storage={0};
    const EffectCaptureNode *target=NULL;
    XrXirStatus status=effect_capture_node_borrow(read,edge->caller,&caller_storage,&caller);
    if(status==XR_XIR_OK) status=effect_capture_node_borrow(read,edge->target,&target_storage,&target);
    if(status!=XR_XIR_OK) return status;
    if(parameter>=target->head.parameter_count) return XR_XIR_BAD_STRUCTURE;
    EffectCaptureActual actual={.node=edge->caller,.value=UINT32_MAX};
    status=effect_capture_kind(read,target,parameter,&actual.kind,NULL);
    if(status==XR_XIR_OK) status=effect_capture_edge_active(read,selected,&actual.rank,&actual.active);
    if(status!=XR_XIR_OK) return status;
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    status=effect_body_request_instruction(&read->body,read->final->nodes[edge->caller].body,edge->instruction,&storage,&op);
    if(status!=XR_XIR_OK) return status;
    EffectInvocationRead owner={.work=read->work,.final=read->final,.body=read->body};
    status=effect_invocation_producer_actual_read(&owner,&read->body,edge,parameter,&actual.value);
    if(status!=XR_XIR_OK) return status;
    if(actual.value==UINT32_MAX) {
        if(op->op==XR_XIR_FUNCTION_REF && edge->latent && parameter>=op->args[1]) actual.unbound=true;
        else if((op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) && !edge->latent &&
            edge->producer<read->final->site_count && parameter<read->final->sites[edge->producer].captures) {
            const EffectInvocationSite *site=&read->final->sites[edge->producer];
            uint64_t binding=caller->basis.origin_words+((uint64_t)site->binding+parameter)*
                (caller->basis.origin_words+(uint64_t)caller->basis.cell_words);
            if(binding>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            actual.value=(uint32_t)op->immediate;actual.captured=true;actual.first=(uint32_t)binding;
            if(actual.kind==EFFECT_CAPTURE_CELL) actual.first+=caller->basis.origin_words;
        } else return XR_XIR_UNRESOLVED;
    } else {
        uint32_t provided=0;status=effect_capture_kind(read,caller,actual.value,&provided,NULL);
        if(status!=XR_XIR_OK) return status;
        if(provided!=actual.kind) return XR_XIR_BAD_TYPE;
        actual.first=actual.kind==EFFECT_CAPTURE_CALLABLE?0:caller->basis.fn_words;
        if(actual.kind==EFFECT_CAPTURE_CELL && (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT)) {
            XrXirType type=0;
            status=effect_body_request_type(&read->body,read->final->nodes[edge->caller].body,(uint32_t)op->immediate,&type);
            if(status!=XR_XIR_OK) return status;
            const XrXirTypeNode *signature=xr_xir_callable_signature(read->body.domain.types,type);
            uint32_t captures=read->final->sites[edge->producer].captures,suffix=parameter-captures;
            if(!signature || captures || suffix>=signature->parameter_count || signature->parameters[suffix].mode!=XR_PARAM_REF ||
                xr_xir_cell_provenance_role(read->final->cells,read->final->nodes[edge->target].body,parameter)!=XR_XIR_CELL_PROOF_SCOPED_REF)
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    if(actual.kind==EFFECT_CAPTURE_CALLABLE) {
        status=effect_capture_argument_bound(read,edge,parameter,actual.captured || edge->latent,&actual);
        if(status!=XR_XIR_OK) return status;
    }
    if(!xir_compile_work(read->work,sizeof(actual))) return XR_XIR_BUDGET;
    *output=actual;return XR_XIR_OK;
}

/* Nested columns need one actual non-leaf origin as well as their own source
 * fact. Empty prefixes wait during construction and cannot close a real use. */
static XrXirStatus effect_capture_nonleaf(const EffectCaptureRead *read,const EffectCaptureNode *node,
    const EffectCaptureActual *actual,bool *present,bool *retain,uint64_t *rank) {
    *present=false;*retain=false;*rank=UINT64_MAX;
    EffectCaptureFact key={actual->node,actual->value,actual->first,0,0};uint32_t at=0;
    XrXirStatus status=effect_capture_lower(read,&key,&at);if(status!=XR_XIR_OK) return status;
    for(;at<read->final->capture.count;++at) {
        if(!xir_compile_work(read->work,6)) return XR_XIR_BUDGET;
        const EffectCaptureFact *fact=&read->final->capture.facts[at];
        if(fact->node!=actual->node || fact->value!=actual->value || fact->word<actual->first ||
            fact->word-actual->first>=node->basis.origin_words) break;
        uint64_t bit=(uint64_t)(fact->word-actual->first)*64+fact->bit;*present=true;
        if(bit>=read->final->site_count || read->final->sites[bit].captures) {
            *retain=true;if(fact->rank<*rank) *rank=fact->rank;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_initial_bit(EffectCaptureRead *read,const EffectCaptureNode *target,
    uint32_t parameter,uint32_t word,uint32_t bit,uint64_t rank) {
    if(word>=target->basis.row_words || bit>=64) return XR_XIR_BAD_STRUCTURE;
    const uint64_t *input=read->final->nodes[target->node].input+(uint64_t)parameter*target->basis.row_words;
    if(!(input[word]&(UINT64_C(1)<<bit))) return XR_XIR_OK;
    return effect_capture_emit(read,(EffectCaptureFact){target->node,target->values+1+parameter,word,bit,rank});
}
static XrXirStatus effect_capture_argument(EffectCaptureRead *read,uint32_t selected,uint32_t parameter) {
    const EffectInvocationEdge *edge=&read->final->edges[selected];EffectCaptureActual actual={0};
    XrXirStatus status=effect_capture_actual(read,selected,parameter,&actual);if(status!=XR_XIR_OK) return status;
    if(!actual.active) return read->builder?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    if(actual.kind==EFFECT_CAPTURE_SCALAR) return XR_XIR_OK;
    EffectCaptureNode target_storage={0};
    const EffectCaptureNode *target=NULL;
    status=effect_capture_node_borrow(read,edge->target,&target_storage,&target);if(status!=XR_XIR_OK) return status;
    uint32_t destination=target->values+1+parameter;
    if(actual.unbound || (actual.captured && actual.kind==EFFECT_CAPTURE_CLASS)) {
        if(actual.kind==EFFECT_CAPTURE_CELL || actual.kind==EFFECT_CAPTURE_CLASS) {
            uint32_t unknown=target->basis.parameters+1;
            for(uint32_t bit=unknown;bit<=unknown+1;++bit) {
                status=effect_capture_initial_bit(read,target,parameter,target->basis.fn_words+bit/64,bit%64,actual.rank+1);
                if(status!=XR_XIR_OK) return status;
            }
        }
    } else {
        uint32_t first=actual.kind==EFFECT_CAPTURE_CALLABLE?0:target->basis.fn_words;
        uint32_t words=actual.kind==EFFECT_CAPTURE_CALLABLE?target->basis.fn_words:target->basis.cell_words;
        if(actual.captured && actual.kind==EFFECT_CAPTURE_CALLABLE) words=target->basis.origin_words;
        EffectCaptureTransfer prefix={.from=actual.node,.value=actual.value,.to=edge->target,.destination=destination,
            .first=actual.first,.word=first,.words=words,.rank=actual.rank,.initial=true};
        status=effect_capture_transfer(read,&prefix);if(status!=XR_XIR_OK) return status;
        if(actual.captured && actual.kind==EFFECT_CAPTURE_CALLABLE) {
            bool present=false,retain=false;uint64_t rank=0;
            status=effect_capture_nonleaf(read,target,&actual,&present,&retain,&rank);if(status!=XR_XIR_OK) return status;
            if(!present && !read->builder) return XR_XIR_UNRESOLVED;
            if(retain) {
                EffectCaptureTransfer nested={.from=actual.node,.value=actual.value,.to=edge->target,.destination=destination,
                    .first=target->basis.origin_words,.word=target->basis.origin_words,
                    .words=target->basis.fn_words-target->basis.origin_words,.rank=rank>actual.rank?rank:actual.rank,.initial=true};
                status=effect_capture_transfer(read,&nested);if(status!=XR_XIR_OK) return status;
            }
        }
    }
    if(actual.kind==EFFECT_CAPTURE_CALLABLE && actual.opaque) {
        uint32_t opaque=read->final->site_count+target->basis.parameters;
        for(uint32_t bit=0;bit<3;++bit) {
            if(bit && !(actual.mask&(bit==1?XR_XIR_CALLABLE_ROOT_REQUIRED:XR_XIR_CALLABLE_ROOT_UNRESOLVED))) continue;
            uint32_t origin=opaque+bit;
            status=effect_capture_initial_bit(read,target,parameter,origin/64,origin%64,actual.rank+1);
            if(status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_capture_argument_word(const EffectCaptureRead *read,const EffectCaptureNode *target,
    const EffectCaptureActual *actual,uint32_t word,uint64_t *output) {
    uint64_t bits=0;
    if(actual->kind==EFFECT_CAPTURE_SCALAR) { *output=0;return XR_XIR_OK; }
    if(actual->unbound || (actual->captured && actual->kind==EFFECT_CAPTURE_CLASS)) {
        if(actual->kind==EFFECT_CAPTURE_CELL || actual->kind==EFFECT_CAPTURE_CLASS) {
            for(uint32_t flag=1;flag<3;++flag) {
                uint32_t bit=target->basis.parameters+flag;
                if(word==target->basis.fn_words+bit/64) bits|=UINT64_C(1)<<(bit%64);
            }
        }
    } else {
        bool source=false;uint32_t selected=word;
        if(actual->kind==EFFECT_CAPTURE_CALLABLE && word<target->basis.fn_words) {
            source=true;
            if(actual->captured) {
                if(word<target->basis.origin_words) selected=actual->first+word;
                else {
                    bool present=false,retain=false;uint64_t rank=0;
                    XrXirStatus status=effect_capture_nonleaf(read,target,actual,&present,&retain,&rank);
                    if(status!=XR_XIR_OK) return status;
                    if(!present) return XR_XIR_UNRESOLVED;
                    source=retain;
                }
            }
        } else if((actual->kind==EFFECT_CAPTURE_CELL || actual->kind==EFFECT_CAPTURE_CLASS) && word>=target->basis.fn_words) {
            source=true;selected=actual->first+word-target->basis.fn_words;
        }
        if(source) {
            XrXirStatus status=effect_capture_word(read,(EffectCaptureFact){actual->node,actual->value,selected,0,0},&bits);
            if(status!=XR_XIR_OK) return status;
        }
    }
    if(actual->kind==EFFECT_CAPTURE_CALLABLE && actual->opaque) {
        uint32_t opaque=read->final->site_count+target->basis.parameters;
        for(uint32_t flag=0;flag<3;++flag) {
            if(flag && !(actual->mask&(flag==1?XR_XIR_CALLABLE_ROOT_REQUIRED:XR_XIR_CALLABLE_ROOT_UNRESOLVED))) continue;
            uint32_t bit=opaque+flag;if(word==bit/64) bits|=UINT64_C(1)<<(bit%64);
        }
    }
    *output=bits;return XR_XIR_OK;
}
static XrXirStatus effect_capture_edge_complete(const EffectCaptureRead *read,uint32_t selected,bool *output) {
    *output=false;const EffectInvocationEdge *edge=&read->final->edges[selected];
    EffectCaptureNode target_storage={0};
    const EffectCaptureNode *target=NULL;
    XrXirStatus status=effect_capture_node_borrow(read,edge->target,&target_storage,&target);if(status!=XR_XIR_OK) return status;
    uint64_t rank=0;bool active=false;status=effect_capture_edge_active(read,selected,&rank,&active);
    if(status!=XR_XIR_OK || !active) return status;
    for(uint32_t p=0;p<target->head.parameter_count;++p) {
        EffectCaptureActual actual={0};status=effect_capture_actual(read,selected,p,&actual);if(status!=XR_XIR_OK) return status;
        for(uint32_t w=0;w<target->basis.row_words;++w) {
            if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
            uint64_t expected=0;status=effect_capture_argument_word(read,target,&actual,w,&expected);
            if(status!=XR_XIR_OK) return status;
            if(expected!=read->final->nodes[edge->target].input[(uint64_t)p*target->basis.row_words+w]) return XR_XIR_OK;
        }
    }
    *output=true;return XR_XIR_OK;
}
static XrXirStatus effect_capture_results(EffectCaptureRead *read,uint32_t selected) {
    const EffectInvocationEdge *edge=&read->final->edges[selected];if(edge->latent) return XR_XIR_OK;
    if(read->selected && edge->caller!=read->selected->node) return XR_XIR_OK;
    EffectCaptureNode caller_storage={0};
    const EffectCaptureNode *caller=NULL;
    EffectCaptureNode target_storage={0};
    const EffectCaptureNode *target=NULL;
    XrXirStatus status=effect_capture_node_borrow(read,edge->caller,&caller_storage,&caller);
    if(status==XR_XIR_OK) status=effect_capture_node_borrow(read,edge->target,&target_storage,&target);
    if(status!=XR_XIR_OK) return status;
    if(read->selected && (read->selected->value<caller->head.parameter_count || read->selected->value>=caller->values)) return XR_XIR_OK;
    uint64_t rank=0;bool active=false;status=effect_capture_edge_active(read,selected,&rank,&active);
    if(status!=XR_XIR_OK || !active) return status;
    for(uint32_t i=0;i<caller->head.instruction_count;++i) {
        if(read->selected && caller->head.parameter_count+i!=read->selected->value) continue;
        if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        status=effect_body_request_instruction(&read->body,read->final->nodes[edge->caller].body,i,&storage,&op);
        if(status!=XR_XIR_OK) return status;
        bool result=(op->op==XR_XIR_INVOKE_RESULT && op->immediate==(int64_t)edge->instruction) ||
            (i==edge->instruction && (op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_CALL_INDIRECT));
        if(!result) continue;
        uint32_t kind=0,provided=0;status=effect_capture_kind(read,caller,caller->head.parameter_count+i,&kind,NULL);
        if(status==XR_XIR_OK) status=effect_capture_kind(read,target,target->values,&provided,NULL);
        if(status!=XR_XIR_OK) return status;
        if(kind!=EFFECT_CAPTURE_CALLABLE && kind!=EFFECT_CAPTURE_CLASS) continue;
        if(kind!=provided) return XR_XIR_BAD_TYPE;
        /* An opaque return boundary retains its advertisement. Identities in
         * the callee's value proof cannot cross that boundary as call targets. */
        if(kind==EFFECT_CAPTURE_CALLABLE) {
            const XrXirTypeNode *signature=xr_xir_callable_signature(read->body.domain.types,target->head.result);
            bool returned=signature && !signature->parameter_span &&
                (signature->flags&XR_XIR_CALLABLE_ROOT_MASK)==XR_XIR_CALLABLE_ROOT_NONE;
            for(uint32_t p=0;signature && p<signature->parameter_count;++p) {
                if(!xir_compile_work(read->work,1)) return XR_XIR_BUDGET;
                if(!xr_xir_callable_parameter_storage_valid(read->body.domain.types,&signature->parameters[p])) return XR_XIR_BAD_TYPE;
                if(signature->parameters[p].mode!=XR_PARAM_READ) returned=false;
            }
            if(!returned) continue;
        }
        if(kind==EFFECT_CAPTURE_CLASS) {
            if(edge->producer!=UINT32_MAX) continue;
            const EffectCaptureFact *raw=NULL;
            status=effect_capture_find(read,(EffectCaptureFact){edge->target,target->values,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,0},&raw);
            if(status!=XR_XIR_OK) return status;
            if(raw) {
                uint64_t distance=raw->rank>rank?raw->rank:rank;
                status=effect_capture_emit(read,(EffectCaptureFact){edge->caller,caller->head.parameter_count+i,
                    EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,distance+1});if(status!=XR_XIR_OK) return status;
            }
            if(!read->class_results) continue;
            bool complete=false;status=effect_capture_edge_complete(read,selected,&complete);
            if(status!=XR_XIR_OK) return status;
            if(!complete) continue;
        }
        uint32_t first=kind==EFFECT_CAPTURE_CALLABLE?0:caller->basis.fn_words;
        uint32_t words=kind==EFFECT_CAPTURE_CALLABLE?caller->basis.fn_words:caller->basis.cell_words;
        EffectCaptureTransfer transfer={.from=edge->target,.value=target->values,.to=edge->caller,
            .destination=caller->head.parameter_count+i,.first=first,.word=first,.words=words,.rank=rank};
        status=effect_capture_transfer(read,&transfer);if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_edges(EffectCaptureRead *read) {
    for(uint32_t e=0;e<read->final->edge_count;++e) {
        if(read->selected && read->supported) return XR_XIR_OK;
        if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
        const EffectInvocationEdge *edge=&read->final->edges[e];
        if(edge->caller>=read->final->count || edge->target>=read->final->count ||
            read->final->nodes[edge->caller].root!=read->final->nodes[edge->target].root) return XR_XIR_BAD_STRUCTURE;
        if(!read->selected || read->selected->node==edge->target) {
            EffectCaptureNode target={0};XrXirStatus status=effect_capture_node(read,edge->target,&target);if(status!=XR_XIR_OK) return status;
            for(uint32_t p=0;p<target.head.parameter_count;++p) {
                if(read->selected && read->selected->value!=target.values+1+p) continue;
                status=effect_capture_argument(read,e,p);if(status!=XR_XIR_OK) return status;
            }
        }
        XrXirStatus status=effect_capture_results(read,e);if(status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_frontier(const EffectCaptureRead *read,uint32_t node,
    uint32_t instruction,uint32_t producer,bool latent) {
    for(uint32_t e=0;e<read->final->edge_count;++e) {
        if(!xir_compile_work(read->work,5)) return XR_XIR_BUDGET;
        const EffectInvocationEdge *edge=&read->final->edges[e];
        if(edge->caller==node && edge->instruction==instruction && edge->producer==producer && edge->latent==latent)
            return XR_XIR_OK;
    }
    return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus effect_capture_frontiers(const EffectCaptureRead *read) {
    for(uint32_t n=0;n<read->final->count;++n) {
        EffectCaptureNode node={0};XrXirStatus status=effect_capture_node(read,n,&node);if(status!=XR_XIR_OK) return status;
        EffectFinalInputSelect cursor;
        status=effect_final_input_select_begin(&read->body,EFFECT_FINAL_SELECT_CAPTURE,&cursor);if(status!=XR_XIR_OK) return status;
        for(;;) {
            XrXirInstruction op;uint32_t i=0;bool found=false;
            status=effect_final_input_select_next(&read->body,read->final->nodes[n].body,&cursor,&i,&op,&found);
            if(status!=XR_XIR_OK) return status;
            if(!found) break;
            uint32_t value=UINT32_MAX;bool latent=op.op==XR_XIR_FUNCTION_REF;
            if(latent) value=node.head.parameter_count+i;
            else if(op.op==XR_XIR_CALL_INDIRECT || op.op==XR_XIR_INVOKE_INDIRECT) {
                if(op.immediate<0 || (uint64_t)op.immediate>=node.values) return XR_XIR_BAD_STRUCTURE;
                value=(uint32_t)op.immediate;
            } else continue;
            EffectCaptureFact key={n,value,0,0,0};uint32_t at=0;
            status=effect_capture_lower(read,&key,&at);if(status!=XR_XIR_OK) return status;
            for(;at<read->final->capture.count;++at) {
                if(!xir_compile_work(read->work,5)) return XR_XIR_BUDGET;
                const EffectCaptureFact *fact=&read->final->capture.facts[at];
                if(fact->node!=n || fact->value!=value || fact->word>=node.basis.origin_words) break;
                uint64_t bit=(uint64_t)fact->word*64+fact->bit;
                if(bit>=read->final->site_count) continue;
                status=effect_capture_frontier(read,n,i,(uint32_t)bit,latent);if(status!=XR_XIR_OK) return status;
            }
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_inputs_match(const EffectCaptureRead *read) {
    for(uint32_t n=0;n<read->final->count;++n) {
        EffectCaptureNode node={0};XrXirStatus status=effect_capture_node(read,n,&node);if(status!=XR_XIR_OK) return status;
        for(uint32_t p=0;p<node.head.parameter_count;++p) for(uint32_t w=0;w<node.basis.row_words;++w) {
            if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
            uint64_t bits=0;status=effect_capture_word(read,(EffectCaptureFact){n,node.values+1+p,w,0,0},&bits);
            if(status!=XR_XIR_OK) return status;
            if(bits!=read->final->nodes[n].input[(uint64_t)p*node.basis.row_words+w]) return XR_XIR_BAD_STRUCTURE;
        }
    }
    for(uint32_t e=0;e<read->final->edge_count;++e) {
        const EffectInvocationEdge *edge=&read->final->edges[e];bool first=true,complete=false;
        for(uint32_t p=0;p<e;++p) {
            if(!xir_compile_work(read->work,5)) return XR_XIR_BUDGET;
            const EffectInvocationEdge *other=&read->final->edges[p];
            if(edge->caller==other->caller && edge->instruction==other->instruction && edge->latent==other->latent &&
                edge->producer==other->producer && read->final->nodes[edge->target].body==read->final->nodes[other->target].body) first=false;
        }
        if(!first) continue;
        for(uint32_t p=e;p<read->final->edge_count && !complete;++p) {
            if(!xir_compile_work(read->work,5)) return XR_XIR_BUDGET;
            const EffectInvocationEdge *other=&read->final->edges[p];
            if(edge->caller!=other->caller || edge->instruction!=other->instruction || edge->latent!=other->latent ||
                edge->producer!=other->producer || read->final->nodes[edge->target].body!=read->final->nodes[other->target].body) continue;
            XrXirStatus status=effect_capture_edge_complete(read,p,&complete);if(status!=XR_XIR_OK) return status;
        }
        if(!complete) return XR_XIR_BAD_STRUCTURE;
    }
    return effect_capture_frontiers(read);
}
