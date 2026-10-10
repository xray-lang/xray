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
    uint32_t target,uint32_t parameter,uint64_t *row) {
    EffectInvocationOwner *owner=flow->owner;
    if (!row || target>=owner->module->function_count ||
        parameter>=owner->module->functions[target].parameter_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *bound=xr_xir_callable_signature(owner->module->types,
        owner->module->functions[target].parameters[parameter]);
    if (!bound) return XR_XIR_OK;
    bool ref=false;
    for (uint32_t a=0;a<bound->parameter_count;++a) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        if (!xr_xir_callable_parameter_storage_valid(owner->module->types,&bound->parameters[a]))
            return XR_XIR_BAD_TYPE;
        if (bound->parameters[a].mode==XR_PARAM_REF) ref=true;
    }
    if (owner->effects->contracts[target].parameters[parameter].kind!=XR_XIR_EFFECT_PARAMETER_FIXED)
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
            uint32_t opaque=owner->site_count+flow->basis.parameters;
            row[opaque/64]|=UINT64_C(1)<<(opaque%64);
            row[(opaque+1)/64]|=UINT64_C(1)<<((opaque+1)%64);
        }
        return XR_XIR_OK;
    }
    if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row)+3)) return XR_XIR_BUDGET;
    memset(row,0,(size_t)flow->basis.fn_words*sizeof(*row));
    uint32_t opaque=owner->site_count+flow->basis.parameters;
    row[opaque/64]|=UINT64_C(1)<<(opaque%64);
    if (mask&XR_XIR_CALLABLE_ROOT_REQUIRED)
        row[(opaque+1)/64]|=UINT64_C(1)<<((opaque+1)%64);
    if (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)
        row[(opaque+2)/64]|=UINT64_C(1)<<((opaque+2)%64);
    return XR_XIR_OK;
}

/* Complete environments are temporary until exact structural admission and
 * deep-copy interning succeed. They never replace a dense physical type key. */
static XrXirStatus effect_invocation_arguments(EffectInvocationFlow *flow,
    uint32_t instruction,uint32_t target,uint32_t producer,
    const uint64_t *closure,uint64_t **output) {
    EffectInvocationOwner *owner=flow->owner;
    if (!output || *output || instruction>=flow->function->instruction_count ||
        target>=owner->module->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&flow->function->instructions[instruction];
    const XrXirFunction *callee=&owner->module->functions[target];
    bool indirect=producer!=UINT32_MAX;uint32_t captures=0,binding=0;
    if (indirect) {
        if (producer>=owner->site_count || !closure) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationSite *site=&owner->sites[producer];
        if (site->target!=target || site->binding>owner->binding_count ||
            site->captures>owner->binding_count-site->binding ||
            !(closure[producer/64]&(UINT64_C(1)<<(producer%64)))) return XR_XIR_BAD_STRUCTURE;
        captures=site->captures;binding=site->binding;
    } else if (closure) return XR_XIR_BAD_STRUCTURE;
    if (captures>callee->parameter_count || op->args[1]!=callee->parameter_count-captures ||
        op->args[0]>flow->function->operand_count ||
        op->args[1]>flow->function->operand_count-op->args[0] ||
        (op->args[1] && !flow->function->operands)) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *signature=NULL;
    if (indirect) {
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->values) return XR_XIR_BAD_VALUE;
        signature=xr_xir_callable_signature(owner->module->types,
            xr_xir_operand_type(flow->function,(uint32_t)op->immediate));
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
            const uint64_t *captured=closure+flow->basis.origin_words+
                (size_t)(binding+p)*(flow->basis.origin_words+flow->basis.cell_words);
            if (xr_xir_type_is_cell(owner->module->types,expected)) {
                if (!xir_compile_work(owner->work,(uint64_t)flow->basis.cell_words*sizeof(*row))) {
                    status=XR_XIR_BUDGET;break;
                }
                memcpy(row+flow->basis.fn_words,captured+flow->basis.origin_words,
                    (size_t)flow->basis.cell_words*sizeof(*row));
            } else if (xr_xir_callable_signature(owner->module->types,expected)) {
                if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row))) {
                    status=XR_XIR_BUDGET;break;
                }
                memcpy(row,captured,(size_t)flow->basis.origin_words*sizeof(*row));
                memcpy(row+flow->basis.origin_words,closure+flow->basis.origin_words,
                    (size_t)(flow->basis.fn_words-flow->basis.origin_words)*sizeof(*row));
                status=effect_invocation_bound_row(flow,target,p,row);
            }
            continue;
        }
        uint32_t suffix=p-captures,actual=flow->function->operands[op->args[0]+suffix];
        if (actual>=flow->values) { status=XR_XIR_BAD_VALUE;break; }
        XrXirType provided=xr_xir_operand_type(flow->function,actual);
        status=effect_invocation_value_matches(owner,expected,provided,&scratch);
        if (status!=XR_XIR_OK) break;
        if (signature) {
            if (!xr_xir_callable_parameter_storage_valid(owner->module->types,&signature->parameters[suffix])) {
                status=XR_XIR_BAD_TYPE;break;
            }
            status=xr_xir_compile_type_substitution_matches_between_scratch(owner->work,
                owner->module->types,owner->module->types,NULL,0,signature->parameters[suffix].type,expected,&scratch);
            if (status!=XR_XIR_OK) break;
        }
        if (xr_xir_type_is_cell(owner->module->types,expected)) {
            if (signature && (captures || signature->parameters[suffix].mode!=XR_PARAM_REF ||
                xr_xir_cell_provenance_role(owner->effects->cells,target,p)!=XR_XIR_CELL_PROOF_SCOPED_REF)) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
            status=effect_invocation_cell(flow,actual,row+flow->basis.fn_words,NULL);
        } else if (xr_xir_callable_signature(owner->module->types,expected)) {
            if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row))) {
                status=XR_XIR_BUDGET;break;
            }
            memcpy(row,flow->rows+(size_t)actual*flow->basis.fn_words,
                (size_t)flow->basis.fn_words*sizeof(*row));
            const XrXirEffectParameter *parameter=&owner->effects->contracts[target].parameters[p];
            bool ref=false;
            const XrXirTypeNode *bound=xr_xir_callable_signature(owner->module->types,expected);
            for (uint32_t a=0;a<bound->parameter_count;++a) {
                if (!xir_compile_work(owner->work,1)) { status=XR_XIR_BUDGET;break; }
                if (bound->parameters[a].mode==XR_PARAM_REF) ref=true;
            }
            if (status!=XR_XIR_OK) break;
            if (flow->fixed[actual] || (parameter->kind==XR_XIR_EFFECT_PARAMETER_FIXED && !ref)) {
                uint32_t mask=0;
                status=effect_invocation_bounds_mask(owner->work,owner->declared,
                    flow->fixed[actual] ? owner->nodes[flow->node].body : target,
                    flow->fixed[actual] ? actual : p,&mask);
                if (status!=XR_XIR_OK) break;
                if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row)+3)) {
                    status=XR_XIR_BUDGET;break;
                }
                memset(row,0,(size_t)flow->basis.fn_words*sizeof(*row));
                uint32_t opaque=owner->site_count+flow->basis.parameters;
                row[opaque/64]|=UINT64_C(1)<<(opaque%64);
                if (mask&XR_XIR_CALLABLE_ROOT_REQUIRED)
                    row[(opaque+1)/64]|=UINT64_C(1)<<((opaque+1)%64);
                if (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)
                    row[(opaque+2)/64]|=UINT64_C(1)<<((opaque+2)%64);
            } else if (parameter->kind==XR_XIR_EFFECT_PARAMETER_FIXED && ref)
                status=effect_invocation_bound_row(flow,target,p,row);
        }
    }
    if (status==XR_XIR_OK && signature)
        status=xr_xir_compile_type_substitution_matches_between_scratch(owner->work,
            owner->module->types,owner->module->types,NULL,0,signature->result,callee->result,&scratch);
    xr_xir_type_match_scratch_free(&scratch);
    if (status!=XR_XIR_OK) { xr_compile_resources_free(input);return status; }
    *output=input;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_connect(EffectInvocationFlow *flow,
    uint32_t instruction,uint32_t target,uint32_t producer,
    const uint64_t *closure,bool *changed) {
    if (!changed) return XR_XIR_BAD_STRUCTURE;
    uint64_t *input=NULL;EffectInvocationOwner *owner=flow->owner;
    XrXirStatus status=effect_invocation_arguments(flow,instruction,target,producer,closure,&input);
    if (status!=XR_XIR_OK) return status;
    uint32_t node=UINT32_MAX,root=owner->nodes[flow->node].root;
    status=effect_invocation_intern(owner,root,target,input,&node);
    xr_compile_resources_free(input);
    if (status==XR_XIR_OK) status=effect_invocation_edge(owner,flow->node,node,instruction,producer,false);
    return status;
}

static XrXirStatus effect_invocation_mask(EffectInvocationFlow *flow,
    uint32_t mask,bool *changed) {
    if (!changed || flow->node>=flow->owner->count ||
        (mask&~(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED)))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->owner->work,3)) return XR_XIR_BUDGET;
    EffectInvocationNode *node=&flow->owner->nodes[flow->node];
    if ((node->intrinsic_mask|mask)!=node->intrinsic_mask) *changed=true;
    node->intrinsic_mask|=mask;
    if (flow->precision) return (mask&~node->local_mask)?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    uint32_t body=node->body;
    for (uint32_t unknown=0;unknown<2;++unknown) {
        uint32_t bit=unknown?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED;
        if (!(mask&bit)) continue;
        XrXirRootEffectWitness witness=flow->instruction==UINT32_MAX ?
            flow->owner->local_witnesses[(size_t)unknown*flow->owner->module->function_count+body] :
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
    if (!changed || flow->node>=flow->owner->count ||
        instruction>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->owner->work,4)) return XR_XIR_BUDGET;
    EffectInvocationNode *node=&flow->owner->nodes[flow->node];
    if (!node->contexts) return XR_XIR_BAD_STRUCTURE;
    if (flow->precision) return node->local_deferred &&
        (node->local_contexts[instruction/64]&(UINT64_C(1)<<(instruction%64)))?
        XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    uint64_t bit=UINT64_C(1)<<(instruction%64);
    if (!node->deferred || !(node->contexts[instruction/64]&bit)) *changed=true;
    node->deferred=true;node->contexts[instruction/64]|=bit;
    XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_CONTEXT_CALL,instruction,UINT32_MAX,UINT32_MAX,0};
    return effect_invocation_local(flow->owner,flow->node,flow->basis.parameters*2+2,witness);
}

static XrXirStatus effect_invocation_has_ref(EffectInvocationFlow *flow,
    const XrXirTypeNode *signature,bool *output) {
    if (!signature || !output || !!signature->parameters!=!!signature->parameter_count)
        return XR_XIR_BAD_TYPE;
    bool result=false;
    for (uint32_t p=0;p<signature->parameter_count;++p) {
        if (!xir_compile_work(flow->owner->work,1)) return XR_XIR_BUDGET;
        if (!xr_xir_callable_parameter_storage_valid(flow->owner->module->types,
                &signature->parameters[p])) return XR_XIR_BAD_TYPE;
        if (signature->parameters[p].mode==XR_PARAM_REF) result=true;
    }
    *output=result;return XR_XIR_OK;
}

/* An unbound REF callable retains the invocation condition, including its
 * actual argument mapping. Ordinary symbolic parameters retain the original
 * PARAMETER obligation; fixed opaque values consume their advertised bound. */
static XrXirStatus effect_invocation_indirect(EffectInvocationFlow *flow,
    uint32_t instruction,bool *changed) {
    EffectInvocationOwner *owner=flow->owner;
    const XrXirInstruction *op=&flow->function->instructions[instruction];
    if ((op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) ||
        op->immediate<0 || (uint64_t)op->immediate>=flow->values)
        return XR_XIR_BAD_VALUE;
    uint32_t value=(uint32_t)op->immediate;
    const XrXirTypeNode *signature=xr_xir_callable_signature(owner->module->types,
        xr_xir_operand_type(flow->function,value));
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
        status=effect_invocation_value_matches(owner,signature->parameters[p].type,
            xr_xir_operand_type(flow->function,actual),&scratch);
    }
    xr_xir_type_match_scratch_free(&scratch);
    if (status!=XR_XIR_OK) return status;
    const uint64_t *descriptor=flow->rows+(size_t)value*flow->basis.fn_words;
    uint32_t opaque=owner->site_count+flow->basis.parameters;
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
    const XrXirFunction *root=&owner->module->functions[owner->nodes[flow->node].root];
    for (uint32_t p=0;p<flow->basis.parameters && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        uint32_t bit=owner->site_count+p;
        if (!(descriptor[bit/64]&(UINT64_C(1)<<(bit%64)))) continue;
        if (!xr_xir_callable_signature(owner->module->types,root->parameters[p]))
            return XR_XIR_BAD_STRUCTURE;
        any=true;
        if (ref) {
            status=effect_invocation_deferred_record(flow,instruction,p,signature);
            if (status==XR_XIR_OK) status=effect_invocation_defer(flow,instruction,changed);
        } else {
            uint32_t root_index=owner->nodes[flow->node].root;
            const XrXirEffectParameter *parameter=&owner->effects->contracts[root_index].parameters[p];
            if (parameter->kind!=XR_XIR_EFFECT_PARAMETER_VARIABLE)
                status=effect_invocation_mask(flow,XR_XIR_CALLABLE_ROOT_UNRESOLVED,changed);
            else {
                if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
                EffectInvocationNode *node=&owner->nodes[flow->node];
                uint64_t dependency=UINT64_C(1)<<(p%64);
                if (!(node->parameters[p/64]&dependency)) *changed=true;
                node->parameters[p/64]|=dependency;
                XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_PARAMETER,instruction,UINT32_MAX,p,0};
                status=flow->precision ?
                    (node->local_parameters[p/64]&dependency ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE) :
                    effect_invocation_local(owner,flow->node,p+2,witness);
            }
        }
    }
    for (uint32_t s=0;s<owner->site_count && status==XR_XIR_OK;++s) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (!(descriptor[s/64]&(UINT64_C(1)<<(s%64)))) continue;
        any=true;
        status=effect_invocation_connect(flow,instruction,owner->sites[s].target,s,
            descriptor,changed);
    }
    if (status==XR_XIR_OK && !any)
        status=effect_invocation_mask(flow,XR_XIR_CALLABLE_ROOT_UNRESOLVED,changed);
    return status;
}

/* Constructing a callable schedules its latent equation but never executes
 * that body in its producer. Unbound suffixes are genuine UNKNOWN inputs;
 * later calls create a separate key with their complete physical actuals. */
static XrXirStatus effect_invocation_latent(EffectInvocationFlow *flow,
    uint32_t instruction) {
    uint32_t value=flow->function->parameter_count+instruction;
    if (value>=flow->values || !flow->certified[value]) return XR_XIR_OK;
    EffectInvocationOwner *owner=flow->owner;uint32_t selected=UINT32_MAX;
    XrXirStatus status=effect_invocation_site(flow,instruction,&selected);
    if (status!=XR_XIR_OK) return status;
    const EffectInvocationSite site=owner->sites[selected];
    const XrXirFunction *target=&owner->module->functions[site.target];
    const XrXirInstruction *op=&flow->function->instructions[instruction];
    uint64_t words=(uint64_t)target->parameter_count*flow->basis.row_words;
    if (words>SIZE_MAX/sizeof(uint64_t)) return XR_XIR_BUDGET;
    uint64_t *input=words ? xir_compile_calloc(owner->work,(size_t)words,sizeof(*input),&status) : NULL;
    if (words && !input) return status;
    for (uint32_t p=0;p<target->parameter_count && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
        uint64_t *row=input+(size_t)p*flow->basis.row_words;
        if (p<site.captures) {
            uint32_t actual=flow->function->operands[op->args[0]+p];
            if (actual>=flow->values) { status=XR_XIR_BAD_VALUE;break; }
            if (xr_xir_type_is_cell(owner->module->types,target->parameters[p]))
                status=effect_invocation_cell(flow,actual,row+flow->basis.fn_words,NULL);
            else if (xr_xir_callable_signature(owner->module->types,target->parameters[p])) {
                if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row))) {
                    status=XR_XIR_BUDGET;break;
                }
                memcpy(row,flow->rows+(size_t)actual*flow->basis.fn_words,
                    (size_t)flow->basis.fn_words*sizeof(*row));
                status=effect_invocation_bound_row(flow,site.target,p,row);
            }
        } else if (xr_xir_type_is_cell(owner->module->types,target->parameters[p])) {
            uint32_t unknown=flow->basis.parameters+1,known=unknown+1;
            row[flow->basis.fn_words+unknown/64]|=UINT64_C(1)<<(unknown%64);
            row[flow->basis.fn_words+known/64]|=UINT64_C(1)<<(known%64);
        } else {
            const XrXirTypeNode *signature=xr_xir_callable_signature(owner->module->types,target->parameters[p]);
            if (!signature) continue;
            uint32_t mask=0;
            status=effect_invocation_bounds_mask(owner->work,owner->declared,site.target,p,&mask);
            if (status!=XR_XIR_OK) break;
            uint32_t opaque=owner->site_count+flow->basis.parameters;
            row[opaque/64]|=UINT64_C(1)<<(opaque%64);
            if (mask&XR_XIR_CALLABLE_ROOT_REQUIRED)
                row[(opaque+1)/64]|=UINT64_C(1)<<((opaque+1)%64);
            if (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)
                row[(opaque+2)/64]|=UINT64_C(1)<<((opaque+2)%64);
        }
    }
    uint32_t node=UINT32_MAX,root=owner->nodes[flow->node].root;
    if (status==XR_XIR_OK) status=effect_invocation_intern(owner,root,site.target,input,&node);
    xr_compile_resources_free(input);
    if (status==XR_XIR_OK) status=effect_invocation_edge(owner,flow->node,node,instruction,selected,true);
    return status;
}

static XrXirStatus effect_invocation_closed_arguments(EffectInvocationFlow *flow,
    uint32_t instruction,uint32_t target,bool *output) {
    if (!output || target>=flow->owner->module->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *callee=&flow->owner->module->functions[target];
    const XrXirInstruction *op=&flow->function->instructions[instruction];
    if (op->args[1]!=callee->parameter_count || op->args[0]>flow->function->operand_count ||
        op->args[1]>flow->function->operand_count-op->args[0] ||
        (op->args[1] && !flow->function->operands)) return XR_XIR_BAD_STRUCTURE;
    bool closed=!xr_xir_type_span(flow->owner->module->types,callee->result);
    for (uint32_t p=0;p<callee->parameter_count;++p) {
        if (!xir_compile_work(flow->owner->work,2)) return XR_XIR_BUDGET;
        uint32_t actual=flow->function->operands[op->args[0]+p];
        if (actual>=flow->values) return XR_XIR_BAD_VALUE;
        if (xr_xir_type_span(flow->owner->module->types,callee->parameters[p]) ||
            xr_xir_type_span(flow->owner->module->types,xr_xir_operand_type(flow->function,actual))) closed=false;
    }
    *output=closed;return XR_XIR_OK;
}

/* GO installs a real actual environment and schedules the target equation
 * for admission queries. It never joins that body into the creating stream. */
static XrXirStatus effect_invocation_transport(EffectInvocationFlow *flow,
    uint32_t instruction,uint32_t target) {
    uint64_t *input=NULL;EffectInvocationOwner *owner=flow->owner;
    XrXirStatus status=effect_invocation_arguments(flow,instruction,target,UINT32_MAX,NULL,&input);
    if (status!=XR_XIR_OK) return status;
    uint32_t node=UINT32_MAX,root=owner->nodes[flow->node].root;
    status=effect_invocation_intern(owner,root,target,input,&node);
    xr_compile_resources_free(input);
    if (status==XR_XIR_OK) status=effect_invocation_edge(owner,flow->node,node,instruction,UINT32_MAX,true);
    return status;
}

static XrXirStatus effect_invocation_instruction(EffectInvocationFlow *flow,
    uint32_t instruction,bool *changed) {
    if (!xir_compile_work(flow->owner->work,1)) return XR_XIR_BUDGET;
    EffectInvocationOwner *owner=flow->owner;
    const XrXirInstruction *op=&flow->function->instructions[instruction];
    switch (op->op) {
    case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_CLEANUP_REGISTER: {
        if (op->immediate<0 || (uint64_t)op->immediate>=owner->module->function_count)
            return XR_XIR_BAD_STRUCTURE;
        uint32_t target=(uint32_t)op->immediate;
        if (op->op==XR_XIR_CLEANUP_REGISTER) {
            const XrXirDeclarations *declarations=owner->module->declarations;
            uint32_t body=owner->nodes[flow->node].body;
            if (!declarations || !declarations->functions ||
                declarations->functions[target].cleanup_owner!=body+1)
                return XR_XIR_BAD_STRUCTURE;
        }
        bool closed=false;
        XrXirStatus status=effect_invocation_closed_arguments(flow,instruction,target,&closed);
        if (status!=XR_XIR_OK) return status;
        if (!closed) return effect_invocation_defer(flow,instruction,changed);
        return effect_invocation_connect(flow,instruction,target,UINT32_MAX,NULL,changed);
    }
    case XR_XIR_CALL_DEFAULT: case XR_XIR_INVOKE_DEFAULT: {
        const uint32_t *identity=xr_xir_default_identity(op);
        const XrXirDefaultBinding *binding=NULL;
        XrXirStatus status=xr_xir_compile_default_lookup(owner->work,owner->module,
            identity[0],identity[1],&binding);
        if (status!=XR_XIR_OK) return status;
        if (!binding || binding->function>=owner->module->function_count ||
            owner->module->functions[binding->function].parameter_count) return XR_XIR_BAD_STRUCTURE;
        if (xr_xir_type_span(owner->module->types,owner->module->functions[binding->function].result))
            return effect_invocation_defer(flow,instruction,changed);
        uint32_t target=UINT32_MAX,root=owner->nodes[flow->node].root;
        status=effect_invocation_intern(owner,root,binding->function,NULL,&target);
        if (status==XR_XIR_OK) status=effect_invocation_edge(owner,flow->node,target,instruction,UINT32_MAX,false);
        return status;
    }
    case XR_XIR_CALL_REQUIREMENT: {
        const XrXirInterfaceTable *table=owner->module->types?owner->module->types->interfaces:NULL;
        if (!table || op->targets[0]>=table->count ||
            op->targets[1]>=table->declarations[op->targets[0]].method_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirTypeNode *signature=xr_xir_callable_signature(owner->module->types,
            table->declarations[op->targets[0]].methods[op->targets[1]].signature);
        if (!signature || !xr_xir_callable_flags_valid(signature->flags)) return XR_XIR_BAD_TYPE;
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        return signature->flags&XR_XIR_CALLABLE_ROOT_UNRESOLVED?
            effect_invocation_defer(flow,instruction,changed):XR_XIR_OK;
    }
    case XR_XIR_GO: {
        if (op->immediate<0 || (uint64_t)op->immediate>=owner->module->function_count)
            return XR_XIR_BAD_STRUCTURE;
        uint32_t target=(uint32_t)op->immediate;bool closed=false;
        XrXirStatus status=effect_invocation_closed_arguments(flow,instruction,target,&closed);
        if (status!=XR_XIR_OK) return status;
        /* Open definition metadata retains its ordinary context obligation;
         * only a complete actual target can enter this private invocation key. */
        if (!closed) return XR_XIR_OK;
        return effect_invocation_transport(flow,instruction,target);
    }
    case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT:
        return effect_invocation_indirect(flow,instruction,changed);
    case XR_XIR_FUNCTION_REF:
        return effect_invocation_latent(flow,instruction);
    case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE: case XR_XIR_CELL_LOCAL_WRITE:
    case XR_XIR_PLACE_READ: case XR_XIR_PLACE_WRITE: {
        if (!owner->effects->cells && (op->op==XR_XIR_PLACE_READ || op->op==XR_XIR_PLACE_WRITE))
            return XR_XIR_OK;
        XrXirStatus status=XR_XIR_OK;
        uint64_t *row=xir_compile_calloc(owner->work,flow->basis.cell_words,sizeof(*row),&status);
        if (!row) return status;
        status=effect_invocation_cell(flow,op->args[0],row,NULL);
        if (status==XR_XIR_OK) status=effect_invocation_cell_consume(flow,row,changed);
        xr_compile_resources_free(row);return status;
    }
    default:
        /* The common exhaustive local seed already classified all opcodes.
         * GO transports a target and does not join its body into this root. */
        return XR_XIR_OK;
    }
}
