/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_calls.inc.c - Actual physical invocation and latent reference edges
 */
static XrXirStatus effect_invocation_bound_row(EffectInvocationFlow *flow,
    uint32_t target,uint32_t parameter,const EffectBodyFunctionHead *head,uint64_t *row) {
    const EffectInvocationRead *owner=flow->read;
    if (!row || !head || target>=flow->body.domain.function_count ||
        parameter>=head->parameter_count || !head->parameters) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *bound=xr_xir_callable_signature(flow->body.domain.types,head->parameters[parameter]);
    if (!bound) return XR_XIR_OK;
    bool ref=false;
    for (uint32_t a=0;a<bound->parameter_count;++a) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        if (!xr_xir_callable_parameter_storage_valid(flow->body.domain.types,&bound->parameters[a]))
            return XR_XIR_BAD_TYPE;
        if (bound->parameters[a].mode==XR_PARAM_REF) ref=true;
    }
    if (effect_contracts_read(owner->effects)[target].parameters[parameter].kind!=XR_XIR_EFFECT_PARAMETER_FIXED)
        return XR_XIR_OK;
    uint32_t mask=0;
    XrXirStatus status=effect_invocation_bounds_mask(owner->work,owner->declared,target,parameter,&mask);
    if (status!=XR_XIR_OK) return status;
    if (ref) {
        /* Actual provenance may close the conditional UNKNOWN portion of a
         * REF callable. Its fixed advertised REQUIRED portion remains real
         * even when a hidden actual target happens to be pure. */
        if (mask&XR_XIR_CALLABLE_ROOT_REQUIRED) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            uint32_t opaque=owner->final->site_count+flow->basis.parameters;
            row[opaque/64]|=UINT64_C(1)<<(opaque%64);
            row[(opaque+1)/64]|=UINT64_C(1)<<((opaque+1)%64);
        }
        return XR_XIR_OK;
    }
    if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row)+3)) return XR_XIR_BUDGET;
    /* Retain the authenticated site relation alongside the original bound. */
    uint32_t opaque=owner->final->site_count+flow->basis.parameters;
    row[opaque/64]|=UINT64_C(1)<<(opaque%64);
    if (mask&XR_XIR_CALLABLE_ROOT_REQUIRED)
        row[(opaque+1)/64]|=UINT64_C(1)<<((opaque+1)%64);
    if (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)
        row[(opaque+2)/64]|=UINT64_C(1)<<((opaque+2)%64);
    return XR_XIR_OK;
}

/* Complete environments are temporary until exact structural admission and
 * deep-copy interning succeed. The caller supplies the complete row read from
 * this request's real ordinal; no subsequent queue visit reuses its address. */
/* Coordinates borrow only this call's genuine target head. The tuple has
 * no authority and cannot outlive its actual stack replay. */
typedef struct EffectInvocationActual {
    uint32_t target,producer;
    const uint64_t *closure;
} EffectInvocationActual;

/* A captured leaf has no environment columns. The enclosing closure's flat
 * relation is not part of that leaf value. Symbolic or owning producers keep
 * their complete relation and remain subject to the ordinary exact reader. */
static XrXirStatus effect_invocation_captured_leaf(EffectInvocationFlow *flow,uint64_t *row) {
    bool concrete=false;
    for(uint32_t w=0;w<flow->basis.origin_words;++w) {
        if(!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
        uint64_t bits=row[w];
        while(bits) {
            if(!xir_compile_work(flow->read->work,3)) return XR_XIR_BUDGET;
            uint64_t producer=(uint64_t)w*64+(uint32_t)__builtin_ctzll(bits);
            bits&=bits-1;
            if(producer>=flow->read->final->site_count || flow->read->final->sites[producer].captures)
                return XR_XIR_OK;
            concrete=true;
        }
    }
    if(!concrete) return XR_XIR_OK;
    uint32_t words=flow->basis.fn_words-flow->basis.origin_words;
    if(!xir_compile_work(flow->read->work,(uint64_t)words*sizeof(*row))) return XR_XIR_BUDGET;
    memset(row+flow->basis.origin_words,0,(size_t)words*sizeof(*row));
    return XR_XIR_OK;
}

/* A prefix projection reads the complete flat relation of this actual value.
 * Nested environments stay in the same finite columns as their producer. */
static XrXirStatus effect_invocation_capture_argument(EffectInvocationFlow *flow,
    const EffectInvocationActual *actual,const EffectBodyFunctionHead *callee,
    uint32_t p,uint64_t *row) {
    const EffectInvocationRead *owner=flow->read;
    const EffectInvocationSite *site=&owner->final->sites[actual->producer];
    const uint64_t *closure=actual->closure;XrXirType expected=callee->parameters[p];
    const uint64_t *captured=closure+flow->basis.origin_words+
        (size_t)(site->binding+p)*(flow->basis.origin_words+flow->basis.cell_words);
    XrXirStatus status=XR_XIR_OK;
    if (xr_xir_type_is_cell(flow->body.domain.types,expected) ||
        xr_xir_type_is_class(flow->body.domain.types,expected)) {
        if (!xir_compile_work(owner->work,(uint64_t)flow->basis.cell_words*sizeof(*row))) return XR_XIR_BUDGET;
        memcpy(row+flow->basis.fn_words,captured+flow->basis.origin_words,
            (size_t)flow->basis.cell_words*sizeof(*row));
    } else if (effect_physical_origin(flow->body.domain.types,expected)) {
        if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row))) return XR_XIR_BUDGET;
        memcpy(row,captured,(size_t)flow->basis.origin_words*sizeof(*row));
        memcpy(row+flow->basis.origin_words,closure+flow->basis.origin_words,
            (size_t)(flow->basis.fn_words-flow->basis.origin_words)*sizeof(*row));
        status=effect_invocation_captured_leaf(flow,row);
        if (status==XR_XIR_OK) status=effect_invocation_bound_row(flow,actual->target,p,callee,row);
    }
    if (status==XR_XIR_OK && xr_xir_type_is_class(flow->body.domain.types,expected)) {
        uint32_t known=flow->basis.parameters+2;
        if (!(row[flow->basis.fn_words+known/64]&(UINT64_C(1)<<(known%64)))) status=XR_XIR_UNRESOLVED;
    }
    if (status==XR_XIR_OK && effect_physical_iterator(flow->body.domain.types,expected)) {
        bool present=false;
        for (uint32_t w=0;w<flow->basis.origin_words;++w) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            present|=row[w]!=0;
        }
        if (!present) status=XR_XIR_UNRESOLVED;
    }
    return status;
}

static XrXirStatus effect_invocation_arguments_read(EffectInvocationFlow *flow,
    const XrXirInstruction *op,const EffectInvocationActual *request,
    const EffectBodyFunctionHead *callee,uint64_t **output) {
    const EffectInvocationRead *owner=flow->read;
    if (!output || *output || !op || !request || !callee ||
        request->target>=flow->body.domain.function_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
    uint32_t target=request->target,producer=request->producer;
    const uint64_t *closure=request->closure;
    bool indirect=producer!=UINT32_MAX,iterator=effect_invocation_iterator_operation(op->op);uint32_t captures=0;
    if (indirect) {
        if (producer>=owner->final->site_count || !closure) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationSite *site=&owner->final->sites[producer];
        if (site->target!=target || site->binding>owner->final->binding_count ||
            site->captures>owner->final->binding_count-site->binding ||
            !(closure[producer/64]&(UINT64_C(1)<<(producer%64)))) return XR_XIR_BAD_STRUCTURE;
        captures=site->captures;
    } else if (closure) return XR_XIR_BAD_STRUCTURE;
    if (iterator && (!indirect || captures!=callee->parameter_count || op->args[0]>=flow->values))
        return XR_XIR_BAD_STRUCTURE;
    if (!iterator && (captures>callee->parameter_count || op->args[1]!=callee->parameter_count-captures ||
        op->args[0]>flow->function->operand_count ||
        op->args[1]>flow->function->operand_count-op->args[0] ||
        (op->args[1] && !flow->function->operands))) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *signature=NULL;
    if (indirect && !iterator) {
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->values) return XR_XIR_BAD_VALUE;
        XrXirType actual_type=0;
        XrXirStatus type_status=effect_invocation_flow_type(flow,(uint32_t)op->immediate,&actual_type);
        if (type_status!=XR_XIR_OK) return type_status;
        signature=xr_xir_callable_signature(flow->body.domain.types,actual_type);
        if (!signature || signature->parameter_count!=callee->parameter_count-captures)
            return XR_XIR_BAD_TYPE;
    }
    uint64_t words=(uint64_t)callee->parameter_count*flow->basis.row_words;
    if (words>SIZE_MAX/sizeof(uint64_t)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    uint64_t *input=words ? xir_compile_calloc(owner->work,(size_t)words,sizeof(*input),&status) : NULL;
    if (words && !input) return status;
    XrXirTypeMatchScratch scratch={owner->work->resources,NULL};
    for (uint32_t p=0;p<callee->parameter_count && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
        uint64_t *row=input+(size_t)p*flow->basis.row_words;
        XrXirType expected=callee->parameters[p];
        if (p<captures) {
            status=effect_invocation_capture_argument(flow,request,callee,p,row);continue;
        }
        uint32_t suffix=p-captures,actual=flow->function->operands[op->args[0]+suffix];
        if (actual>=flow->values) { status=XR_XIR_BAD_VALUE;break; }
        XrXirType provided=0;status=effect_invocation_flow_type(flow,actual,&provided);
        if (status!=XR_XIR_OK) break;
        status=effect_invocation_value_matches(flow,expected,provided,&scratch);
        if (status!=XR_XIR_OK) break;
        if (signature) {
            if (!xr_xir_callable_parameter_storage_valid(flow->body.domain.types,&signature->parameters[suffix])) {
                status=XR_XIR_BAD_TYPE;break;
            }
            status=xr_xir_compile_type_substitution_matches_between_scratch(owner->work,
                flow->body.domain.types,flow->body.domain.types,NULL,0,signature->parameters[suffix].type,expected,&scratch);
            if (status!=XR_XIR_OK) break;
        }
        if (xr_xir_type_is_cell(flow->body.domain.types,expected)) {
            if (signature && (captures || signature->parameters[suffix].mode!=XR_PARAM_REF ||
                xr_xir_cell_provenance_role(owner->final->cells,target,p)!=XR_XIR_CELL_PROOF_SCOPED_REF)) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
            status=effect_invocation_cell(flow,actual,row+flow->basis.fn_words,NULL);
        } else if (xr_xir_type_is_class(flow->body.domain.types,expected)) {
            status=effect_invocation_class_word(flow,actual,row+flow->basis.fn_words);
        } else if (effect_physical_origin(flow->body.domain.types,expected)) {
            if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row))) {
                status=XR_XIR_BUDGET;break;
            }
            const uint64_t *actual_row=effect_invocation_row(flow,actual);
            if (!actual_row) { status=XR_XIR_BAD_STRUCTURE;break; }
            memcpy(row,actual_row,
                (size_t)flow->basis.fn_words*sizeof(*row));
            if(effect_physical_iterator(flow->body.domain.types,expected)) {
                bool present=false;
                for (uint32_t w=0;w<flow->basis.origin_words;++w) {
                    if (!xir_compile_work(owner->work,1)) {status=XR_XIR_BUDGET;break;}
                    present|=row[w]!=0;
                }
                if (status==XR_XIR_OK && !present) status=XR_XIR_UNRESOLVED;
                continue;
            }
            const XrXirEffectParameter *parameter=&effect_contracts_read(owner->effects)[target].parameters[p];
            bool ref=false;
            const XrXirTypeNode *bound=xr_xir_callable_signature(flow->body.domain.types,expected);
            for (uint32_t a=0;a<bound->parameter_count;++a) {
                if (!xir_compile_work(owner->work,1)) { status=XR_XIR_BUDGET;break; }
                if (bound->parameters[a].mode==XR_PARAM_REF) ref=true;
            }
            if (status!=XR_XIR_OK) break;
            bool fixed=false;status=effect_invocation_flag_get(flow,actual,false,&fixed);
            if (status!=XR_XIR_OK) break;
            if (fixed || (parameter->kind==XR_XIR_EFFECT_PARAMETER_FIXED && !ref)) {
                uint32_t mask=0;
                status=effect_invocation_bounds_mask(owner->work,owner->declared,
                    fixed ? owner->final->nodes[flow->node].body : target,
                    fixed ? actual : p,&mask);
                if (status!=XR_XIR_OK) break;
                if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row)+3)) {
                    status=XR_XIR_BUDGET;break;
                }
                /* The fixed advertisement never erases the actual producer relation. */
                uint32_t opaque=owner->final->site_count+flow->basis.parameters;
                row[opaque/64]|=UINT64_C(1)<<(opaque%64);
                if (mask&XR_XIR_CALLABLE_ROOT_REQUIRED)
                    row[(opaque+1)/64]|=UINT64_C(1)<<((opaque+1)%64);
                if (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)
                    row[(opaque+2)/64]|=UINT64_C(1)<<((opaque+2)%64);
            } else if (parameter->kind==XR_XIR_EFFECT_PARAMETER_FIXED && ref)
                status=effect_invocation_bound_row(flow,target,p,callee,row);
        }
    }
    if (status==XR_XIR_OK && signature)
        status=xr_xir_compile_type_substitution_matches_between_scratch(owner->work,
            flow->body.domain.types,flow->body.domain.types,NULL,0,signature->result,callee->result,&scratch);
    xr_xir_type_match_scratch_free(&scratch);
    if (status!=XR_XIR_OK) { xr_compile_resources_free(input);return status; }
    *output=input;return XR_XIR_OK;
}

/* Consumers without a current head perform the same complete status read.
 * The argument algorithm is shared; no cached role or permission is accepted. */
static XrXirStatus effect_invocation_arguments(EffectInvocationFlow *flow,
    const XrXirInstruction *op,uint32_t target,uint32_t producer,
    const uint64_t *closure,uint64_t **output) {
    if (!output || *output || !op || target>=flow->body.domain.function_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionHead head;
    XrXirStatus status=effect_invocation_flow_head(flow,target,&head);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(flow->read->work,sizeof(EffectInvocationActual))) return XR_XIR_BUDGET;
    EffectInvocationActual actual={target,producer,closure};
    return effect_invocation_arguments_read(flow,op,&actual,&head,output);
}

static XrXirStatus effect_invocation_connect(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *op,const EffectInvocationActual *actual,
    const EffectBodyFunctionHead *head,bool *changed) {
    if (!changed || !op || !actual || !head || instruction>=flow->function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationRead *owner=flow->read;uint32_t producer=actual->producer;
    if (producer!=UINT32_MAX && producer>=owner->final->site_count) return XR_XIR_BAD_STRUCTURE;
    if (producer==UINT32_MAX && (op->immediate<0 ||
        (uint64_t)op->immediate>=flow->body.domain.function_count)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
    uint32_t target=producer==UINT32_MAX?(uint32_t)op->immediate:owner->final->sites[producer].target;
    if (actual->target!=target) return XR_XIR_BAD_STRUCTURE;
    uint64_t *input=NULL;
    XrXirStatus status=effect_invocation_arguments_read(flow,op,actual,head,&input);
    if (status==XR_XIR_UNRESOLVED) return XR_XIR_OK;
    if (status!=XR_XIR_OK) return status;
    uint32_t node=UINT32_MAX,root=owner->final->nodes[flow->node].root;
    status=effect_invocation_intern(flow->owner,root,target,input,&node);
    xr_compile_resources_free(input);
    if (status==XR_XIR_OK) status=effect_invocation_edge(flow->owner,flow->node,node,instruction,producer,false);
    return status;
}

static XrXirStatus effect_invocation_mask(EffectInvocationFlow *flow,
    uint32_t mask,bool *changed) {
    if (!changed || flow->node>=flow->read->final->count ||
        (mask&~(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED)))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->read->work,3)) return XR_XIR_BUDGET;
    EffectInvocationNode *node=&flow->read->final->nodes[flow->node];
    if ((node->intrinsic_mask|mask)!=node->intrinsic_mask) *changed=true;
    node->intrinsic_mask|=mask;
    if (flow->precision) return (mask&~node->local_mask)?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    uint32_t body=node->body;
    for (uint32_t unknown=0;unknown<2;++unknown) {
        uint32_t bit=unknown?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED;
        if (!(mask&bit)) continue;
        XrXirRootEffectWitness witness=flow->instruction==UINT32_MAX ?
            flow->read->final->local_witnesses[(size_t)unknown*flow->body.domain.function_count+body] :
            (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT,flow->instruction,UINT32_MAX,UINT32_MAX,0};
        XrXirStatus status=effect_invocation_local(flow->owner,flow->node,unknown,witness);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* A deferred call is indexed by its actual local instruction. Propagation
 * projects it to the caller's real frontier rather than copying a child ID. */
static XrXirStatus effect_invocation_defer(EffectInvocationFlow *flow,
    uint32_t instruction,bool *changed) {
    if (!changed || flow->node>=flow->read->final->count ||
        instruction>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->read->work,4)) return XR_XIR_BUDGET;
    EffectInvocationNode *node=&flow->read->final->nodes[flow->node];
    if (!effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,false)) return XR_XIR_BAD_STRUCTURE;
    if (flow->precision) return node->local_deferred &&
        (effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,true)[instruction/64]&(UINT64_C(1)<<(instruction%64)))?
        XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    uint64_t bit=UINT64_C(1)<<(instruction%64);
    if (!node->deferred || !(effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,false)[instruction/64]&bit)) *changed=true;
    node->deferred=true;effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,false)[instruction/64]|=bit;
    XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_CONTEXT_CALL,instruction,UINT32_MAX,UINT32_MAX,0};
    return effect_invocation_local(flow->owner,flow->node,flow->basis.parameters*2+2,witness);
}

static XrXirStatus effect_invocation_has_ref(EffectInvocationFlow *flow,
    const XrXirTypeNode *signature,bool *output) {
    if (!signature || !output || !!signature->parameters!=!!signature->parameter_count)
        return XR_XIR_BAD_TYPE;
    bool result=false;
    for (uint32_t p=0;p<signature->parameter_count;++p) {
        if (!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
        if (!xr_xir_callable_parameter_storage_valid(flow->body.domain.types,
                &signature->parameters[p])) return XR_XIR_BAD_TYPE;
        if (signature->parameters[p].mode==XR_PARAM_REF) result=true;
    }
    *output=result;return XR_XIR_OK;
}

/* An unbound REF callable retains the invocation condition, including its
 * actual argument mapping. Ordinary symbolic parameters retain the original
 * PARAMETER obligation; fixed opaque values consume their advertised bound. */
static XrXirStatus effect_invocation_indirect(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *op,bool *changed) {
    const EffectInvocationRead *owner=flow->read;
    if (!op) return XR_XIR_BAD_STRUCTURE;
    if ((op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) ||
        op->immediate<0 || (uint64_t)op->immediate>=flow->values)
        return XR_XIR_BAD_VALUE;
    uint32_t value=(uint32_t)op->immediate;
    XrXirType actual_type=0;
    XrXirStatus type_status=effect_invocation_flow_type(flow,value,&actual_type);
    if (type_status!=XR_XIR_OK) return type_status;
    const XrXirTypeNode *signature=xr_xir_callable_signature(flow->body.domain.types,actual_type);
    bool ref=false;
    XrXirStatus status=effect_invocation_has_ref(flow,signature,&ref);
    if (status!=XR_XIR_OK) return status;
    if (op->args[1]!=signature->parameter_count ||
        op->args[0]>flow->function->operand_count ||
        op->args[1]>flow->function->operand_count-op->args[0] ||
        (op->args[1] && !flow->function->operands)) return XR_XIR_BAD_STRUCTURE;
    XrXirTypeMatchScratch scratch={owner->work->resources,NULL};
    for (uint32_t p=0;p<signature->parameter_count && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(owner->work,1)) { status=XR_XIR_BUDGET;break; }
        uint32_t actual=flow->function->operands[op->args[0]+p];
        if (actual>=flow->values) { status=XR_XIR_BAD_VALUE;break; }
        XrXirType provided=0;status=effect_invocation_flow_type(flow,actual,&provided);
        if (status==XR_XIR_OK)
            status=effect_invocation_value_matches(flow,signature->parameters[p].type,provided,&scratch);
    }
    xr_xir_type_match_scratch_free(&scratch);
    if (status!=XR_XIR_OK) return status;
    const uint64_t *descriptor=effect_invocation_row(flow,value);
    if (!descriptor) return XR_XIR_BAD_STRUCTURE;
    uint32_t opaque=owner->final->site_count+flow->basis.parameters;
    bool any=false;
    if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
    bool opaque_present=(descriptor[opaque/64]&(UINT64_C(1)<<(opaque%64)))!=0;
    uint32_t mask=0;
    if (descriptor[(opaque+1)/64]&(UINT64_C(1)<<((opaque+1)%64)))
        mask|=XR_XIR_CALLABLE_ROOT_REQUIRED;
    if (descriptor[(opaque+2)/64]&(UINT64_C(1)<<((opaque+2)%64)))
        mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    if (mask && !opaque_present) return XR_XIR_BAD_STRUCTURE;
    if (opaque_present) {
        any=true;status=effect_invocation_mask(flow,mask,changed);
    }
    EffectBodyFunctionSignature root_head={0};
    status=effect_body_request_signature(&flow->body,owner->final->nodes[flow->node].root,&root_head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionSignature *root=&root_head;
    for (uint32_t p=0;p<flow->basis.parameters && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        uint32_t bit=owner->final->site_count+p;
        if (!(descriptor[bit/64]&(UINT64_C(1)<<(bit%64)))) continue;
        if (!xr_xir_callable_signature(flow->body.domain.types,root->parameters[p]))
            return XR_XIR_BAD_STRUCTURE;
        any=true;
        if (ref) {
            status=effect_invocation_deferred_record(flow,instruction,p,signature);
            if (status==XR_XIR_OK) status=effect_invocation_defer(flow,instruction,changed);
        } else {
            uint32_t root_index=owner->final->nodes[flow->node].root;
            const XrXirEffectParameter *parameter=&effect_contracts_read(owner->effects)[root_index].parameters[p];
            if (parameter->kind!=XR_XIR_EFFECT_PARAMETER_VARIABLE)
                status=effect_invocation_mask(flow,XR_XIR_CALLABLE_ROOT_UNRESOLVED,changed);
            else {
                if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
                EffectInvocationNode *node=&owner->final->nodes[flow->node];
                uint64_t dependency=UINT64_C(1)<<(p%64);
                if (!(effect_invocation_node_row(node,EFFECT_INVOCATION_PARAMETERS,false)[p/64]&dependency)) *changed=true;
                effect_invocation_node_row(node,EFFECT_INVOCATION_PARAMETERS,false)[p/64]|=dependency;
                XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_PARAMETER,instruction,UINT32_MAX,p,0};
                status=flow->precision ?
                    (effect_invocation_node_row(node,EFFECT_INVOCATION_PARAMETERS,true)[p/64]&dependency ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE) :
                    effect_invocation_local(flow->owner,flow->node,p+2,witness);
            }
        }
    }
    for (uint32_t s=0;s<owner->final->site_count && status==XR_XIR_OK;++s) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (!(descriptor[s/64]&(UINT64_C(1)<<(s%64)))) continue;
        any=true;
        EffectBodyFunctionHead head;
        status=effect_invocation_flow_head(flow,owner->final->sites[s].target,&head);
        if (status!=XR_XIR_OK) break;
        if (!xir_compile_work(owner->work,sizeof(EffectInvocationActual))) return XR_XIR_BUDGET;
        EffectInvocationActual actual={owner->final->sites[s].target,s,descriptor};
        status=effect_invocation_connect(flow,instruction,op,&actual,&head,changed);
    }
    if (status==XR_XIR_OK && !any)
        status=effect_invocation_mask(flow,XR_XIR_CALLABLE_ROOT_UNRESOLVED,changed);
    return status;
}

/* Constructing a callable schedules its latent equation but never executes
 * that body in its producer. Unbound suffixes are genuine UNKNOWN inputs;
 * later calls create a separate key with their complete physical actuals. */
/* Replay the original complete latent input without linking another key.
 * Unbound suffixes remain opaque; captured inputs keep their physical rows. */
static XrXirStatus effect_invocation_latent_input(EffectInvocationFlow *flow,
    uint32_t instruction,uint32_t selected,const XrXirInstruction *op,uint64_t **output) {
    const EffectInvocationRead *owner=flow->read;
    if (!output || *output || !op || selected>=owner->final->site_count ||
        instruction>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    const EffectInvocationSite site=owner->final->sites[selected];
    EffectBodyFunctionHead target_head={0};
    status=effect_invocation_flow_head(flow,site.target,&target_head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionHead *target=&target_head;
    uint64_t words=(uint64_t)target->parameter_count*flow->basis.row_words;
    if (words>SIZE_MAX/sizeof(uint64_t)) return XR_XIR_BUDGET;
    uint64_t *input=words ? xir_compile_calloc(owner->work,(size_t)words,sizeof(*input),&status) : NULL;
    if (words && !input) return status;
    for (uint32_t p=0;p<target->parameter_count && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
        uint64_t *row=input+(size_t)p*flow->basis.row_words;
        if (xr_xir_type_is_class(flow->body.domain.types,target->parameters[p])) {
            if(p<site.captures) {
                uint32_t actual=flow->function->operands[op->args[0]+p];
                status=effect_invocation_class_word(flow,actual,row+flow->basis.fn_words);
            } else status=effect_invocation_class_encode(flow,EFFECT_CLASS_KNOWN|EFFECT_CLASS_UNKNOWN,
                row+flow->basis.fn_words);
            continue;
        }
        if (p<site.captures) {
            uint32_t actual=flow->function->operands[op->args[0]+p];
            if (actual>=flow->values) { status=XR_XIR_BAD_VALUE;break; }
            if (xr_xir_type_is_cell(flow->body.domain.types,target->parameters[p]))
                status=effect_invocation_cell(flow,actual,row+flow->basis.fn_words,NULL);
            else if (effect_physical_origin(flow->body.domain.types,target->parameters[p])) {
                if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row))) {
                    status=XR_XIR_BUDGET;break;
                }
                const uint64_t *actual_row=effect_invocation_row(flow,actual);
                if (!actual_row) { status=XR_XIR_BAD_STRUCTURE;break; }
                memcpy(row,actual_row,
                    (size_t)flow->basis.fn_words*sizeof(*row));
                status=effect_invocation_bound_row(flow,site.target,p,target,row);
            }
        } else if (xr_xir_type_is_cell(flow->body.domain.types,target->parameters[p])) {
            uint32_t unknown=flow->basis.parameters+1,known=unknown+1;
            row[flow->basis.fn_words+unknown/64]|=UINT64_C(1)<<(unknown%64);
            row[flow->basis.fn_words+known/64]|=UINT64_C(1)<<(known%64);
        } else {
            const XrXirTypeNode *signature=xr_xir_callable_signature(flow->body.domain.types,target->parameters[p]);
            if (!signature) continue;
            uint32_t mask=0;
            status=effect_invocation_bounds_mask(owner->work,owner->declared,site.target,p,&mask);
            if (status!=XR_XIR_OK) break;
            uint32_t opaque=owner->final->site_count+flow->basis.parameters;
            row[opaque/64]|=UINT64_C(1)<<(opaque%64);
            if (mask&XR_XIR_CALLABLE_ROOT_REQUIRED)
                row[(opaque+1)/64]|=UINT64_C(1)<<((opaque+1)%64);
            if (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)
                row[(opaque+2)/64]|=UINT64_C(1)<<((opaque+2)%64);
        }
    }
    if (status!=XR_XIR_OK) { xr_compile_resources_free(input);return status; }
    *output=input;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_latent(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *op) {
    uint32_t value=flow->function->parameter_count+instruction;
    if (value>=flow->values) return XR_XIR_OK;
    bool certified=false;XrXirStatus status=effect_invocation_flag_get(flow,value,true,&certified);
    if (status!=XR_XIR_OK || !certified) return status;
    const EffectInvocationRead *owner=flow->read;uint32_t selected=UINT32_MAX;
    status=effect_invocation_site(flow,instruction,&selected);
    if (status!=XR_XIR_OK) return status;
    uint64_t *input=NULL;
    status=effect_invocation_latent_input(flow,instruction,selected,op,&input);
    uint32_t node=UINT32_MAX,root=owner->final->nodes[flow->node].root;
    if (status==XR_XIR_OK) status=effect_invocation_intern(flow->owner,root,owner->final->sites[selected].target,input,&node);
    xr_compile_resources_free(input);
    if (status==XR_XIR_OK) status=effect_invocation_edge(flow->owner,flow->node,node,instruction,selected,true);
    return status==XR_XIR_UNRESOLVED?XR_XIR_OK:status;
}

static XrXirStatus effect_invocation_closed_arguments(EffectInvocationFlow *flow,
    uint32_t target,const XrXirInstruction *op,bool *output,EffectBodyFunctionHead *callee) {
    if (!output || !op || !callee || target>=flow->body.domain.function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus head_status=effect_invocation_flow_head(flow,target,callee);
    if (head_status!=XR_XIR_OK) return head_status;
    if (op->args[1]!=callee->parameter_count || op->args[0]>flow->function->operand_count ||
        op->args[1]>flow->function->operand_count-op->args[0] ||
        (op->args[1] && !flow->function->operands)) return XR_XIR_BAD_STRUCTURE;
    bool closed=!xr_xir_type_span(flow->body.domain.types,callee->result);
    for (uint32_t p=0;p<callee->parameter_count;++p) {
        if (!xir_compile_work(flow->read->work,2)) return XR_XIR_BUDGET;
        uint32_t actual=flow->function->operands[op->args[0]+p];
        if (actual>=flow->values) return XR_XIR_BAD_VALUE;
        XrXirType provided=0;
        XrXirStatus type_status=effect_invocation_flow_type(flow,actual,&provided);
        if (type_status!=XR_XIR_OK) return type_status;
        if (xr_xir_type_span(flow->body.domain.types,callee->parameters[p]) ||
            xr_xir_type_span(flow->body.domain.types,provided)) closed=false;
    }
    *output=closed;return XR_XIR_OK;
}

/* GO installs a real actual environment and schedules the target equation
 * for admission queries. It never joins that body into the creating stream. */
static XrXirStatus effect_invocation_transport(EffectInvocationFlow *flow,
    uint32_t instruction,uint32_t target,const XrXirInstruction *op,const EffectBodyFunctionHead *head) {
    if (!op || !head || instruction>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    uint64_t *input=NULL;const EffectInvocationRead *owner=flow->read;
    if (!xir_compile_work(owner->work,sizeof(EffectInvocationActual))) return XR_XIR_BUDGET;
    EffectInvocationActual actual={target,UINT32_MAX,NULL};
    XrXirStatus status=effect_invocation_arguments_read(flow,op,&actual,head,&input);
    if (status==XR_XIR_UNRESOLVED) return XR_XIR_OK;
    if (status!=XR_XIR_OK) return status;
    uint32_t node=UINT32_MAX,root=owner->final->nodes[flow->node].root;
    status=effect_invocation_intern(flow->owner,root,target,input,&node);
    xr_compile_resources_free(input);
    if (status==XR_XIR_OK) status=effect_invocation_edge(flow->owner,flow->node,node,instruction,UINT32_MAX,true);
    return status;
}

#include "xxir_effect_invocation_iterators.inc.c"

/* The caller owns this complete decoded stack row for only this request.
 * It was read from the same BodyRequest at this actual ordinal; it is never
 * retained by an equation, descriptor or another queue visit. */
static XrXirStatus effect_invocation_instruction_decoded(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *op,bool *changed) {
    if (!flow || !flow->owner || !flow->function || !op || !changed ||
        instruction>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->read->work,2)) return XR_XIR_BUDGET;
    const EffectInvocationRead *owner=flow->read;
    switch (op->op) {
    case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_CLEANUP_REGISTER: {
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->body.domain.function_count)
            return XR_XIR_BAD_STRUCTURE;
        uint32_t target=(uint32_t)op->immediate;
        if (op->op==XR_XIR_CLEANUP_REGISTER) {
            const XrXirDeclarations *declarations=flow->body.domain.declarations;
            uint32_t body=owner->final->nodes[flow->node].body;
            if (!declarations || !declarations->functions ||
                declarations->functions[target].cleanup_owner!=body+1)
                return XR_XIR_BAD_STRUCTURE;
        }
        bool closed=false;EffectBodyFunctionHead head;
        XrXirStatus status=effect_invocation_closed_arguments(flow,target,op,&closed,&head);
        if (status!=XR_XIR_OK) return status;
        if (!closed) return effect_invocation_defer(flow,instruction,changed);
        if (!xir_compile_work(owner->work,sizeof(EffectInvocationActual))) return XR_XIR_BUDGET;
        EffectInvocationActual actual={target,UINT32_MAX,NULL};
        return effect_invocation_connect(flow,instruction,op,&actual,&head,changed);
    }
    case XR_XIR_CALL_DEFAULT: case XR_XIR_INVOKE_DEFAULT: {
        const uint32_t *identity=xr_xir_default_identity(op);
        const XrXirDefaultBinding *binding=NULL;
        XrXirStatus status=xir_metadata_default_lookup(owner->work,&flow->body.domain,
            identity[0],identity[1],&binding);
        if (status!=XR_XIR_OK) return status;
        if (!binding || binding->function>=flow->body.domain.function_count) return XR_XIR_BAD_STRUCTURE;
        EffectBodyFunctionSignature head={0};
        status=effect_body_request_signature(&flow->body,binding->function,&head);
        if (status!=XR_XIR_OK) return status;
        if (head.parameter_count) return XR_XIR_BAD_STRUCTURE;
        if (xr_xir_type_span(flow->body.domain.types,head.result))
            return effect_invocation_defer(flow,instruction,changed);
        uint32_t target=UINT32_MAX,root=owner->final->nodes[flow->node].root;
        status=effect_invocation_intern(flow->owner,root,binding->function,NULL,&target);
        if (status==XR_XIR_OK) status=effect_invocation_edge(flow->owner,flow->node,target,instruction,UINT32_MAX,false);
        return status;
    }
    case XR_XIR_CALL_REQUIREMENT: {
        const XrXirInterfaceTable *table=flow->body.domain.types?flow->body.domain.types->interfaces:NULL;
        if (!table || op->targets[0]>=table->count ||
            op->targets[1]>=table->declarations[op->targets[0]].method_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirTypeNode *signature=xr_xir_callable_signature(flow->body.domain.types,
            table->declarations[op->targets[0]].methods[op->targets[1]].signature);
        if (!signature || !xr_xir_callable_flags_valid(signature->flags)) return XR_XIR_BAD_TYPE;
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        return signature->flags&XR_XIR_CALLABLE_ROOT_UNRESOLVED?
            effect_invocation_defer(flow,instruction,changed):XR_XIR_OK;
    }
    case XR_XIR_GO: case XR_XIR_GENERATOR_NEW: {
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->body.domain.function_count)
            return XR_XIR_BAD_STRUCTURE;
        uint32_t target=(uint32_t)op->immediate;bool closed=false;EffectBodyFunctionHead head;
        XrXirStatus status=effect_invocation_closed_arguments(flow,target,op,&closed,&head);
        if (status!=XR_XIR_OK) return status;
        /* Open definition metadata retains its ordinary context obligation;
         * only a complete actual target can enter this private invocation key. */
        if (!closed) return XR_XIR_OK;
        return effect_invocation_transport(flow,instruction,target,op,&head);
    }
    case XR_XIR_ITERATOR_HAS_NEXT: case XR_XIR_ITERATOR_NEXT: case XR_XIR_ITERATOR_NTH:
        return effect_invocation_iterator(flow,instruction,op,changed);
    case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT:
        return effect_invocation_indirect(flow,instruction,op,changed);
    case XR_XIR_FUNCTION_REF:
        return effect_invocation_latent(flow,instruction,op);
    case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE: case XR_XIR_CELL_LOCAL_WRITE:
    case XR_XIR_PLACE_READ: case XR_XIR_PLACE_WRITE: case XR_XIR_CELL_PROJECT: {
        if (!owner->final->cells && (op->op==XR_XIR_PLACE_READ || op->op==XR_XIR_PLACE_WRITE))
            return XR_XIR_OK;
        XrXirStatus status=XR_XIR_OK;
        uint64_t *row=xir_compile_calloc(owner->work,flow->basis.cell_words,sizeof(*row),&status);
        if (!row) return status;
        status=effect_invocation_cell(flow,op->args[0],row,NULL);
        if (status==XR_XIR_OK) status=effect_invocation_cell_consume(flow,row,changed);
        xr_compile_resources_free(row);return status==XR_XIR_UNRESOLVED?XR_XIR_OK:status;
    }
    default:
        /* The common exhaustive local seed already classified all opcodes.
         * GO transports a target and does not join its body into this root. */
        return XR_XIR_OK;
    }
}

/* Callers without a decoded row perform the original complete status-returning
 * read. A failed callback or canonical record cannot reach the dispatcher. */
static XrXirStatus effect_invocation_instruction(EffectInvocationFlow *flow,
    uint32_t instruction,bool *changed) {
    if (!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    XrXirStatus status=effect_invocation_flow_instruction(flow,instruction,&storage,&op);
    if (status!=XR_XIR_OK) return status;
    return effect_invocation_instruction_decoded(flow,instruction,op,changed);
}
