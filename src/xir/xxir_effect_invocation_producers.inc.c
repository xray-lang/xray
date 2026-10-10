/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_producers.inc.c - Rooted producer facts on the existing equation queue
 *
 * KEY CONCEPT:
 *   Original scalar and owned Cell captures cross ordinary NONE returns.
 *   Sparse SSA facts use the fixed site basis and descending authentic causes.
 *   They supplement an advertisement; they never replace its ROOT mask.
 */
static XrXirStatus effect_invocation_producer_return(const EffectInvocationOwner *owner,
    uint32_t body,bool *output) {
    if (!owner || !output || body>=owner->module->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *signature=xr_xir_callable_signature(owner->module->types,
        owner->module->functions[body].result);
    if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
    if (signature && (!xr_xir_callable_flags_valid(signature->flags) ||
        (!!signature->parameters!=!!signature->parameter_count))) return XR_XIR_BAD_TYPE;
    bool supported=signature && !signature->parameter_span &&
        (signature->flags&XR_XIR_CALLABLE_ROOT_MASK)==XR_XIR_CALLABLE_ROOT_NONE;
    for (uint32_t p=0;signature && p<signature->parameter_count;++p) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        if (!xr_xir_callable_parameter_storage_valid(owner->module->types,&signature->parameters[p]))
            return XR_XIR_BAD_TYPE;
        if (signature->parameters[p].mode!=XR_PARAM_READ) supported=false;
    }
    *output=supported;return XR_XIR_OK;
}

/* Only builtin scalar and authentic owned Cell prefixes participate. A
 * captured callable or aggregate needs its complete relation rather than an
 * invented empty environment, so no identity fact is emitted for that site. */
static XrXirStatus effect_invocation_producer_site_supported(const EffectInvocationOwner *owner,
    uint32_t index,bool *output) {
    if (!owner || !output || index>=owner->site_count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationSite *site=&owner->sites[index];
    if (site->target>=owner->module->function_count || site->function>=owner->module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *target=&owner->module->functions[site->target];
    const XrXirFunction *producer=&owner->module->functions[site->function];
    if (site->captures>target->parameter_count || site->instruction>=producer->instruction_count ||
        site->binding>owner->binding_count || site->captures>owner->binding_count-site->binding)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&producer->instructions[site->instruction];
    if (op->op!=XR_XIR_FUNCTION_REF || op->immediate!=(int64_t)site->target || op->args[1]!=site->captures)
        return XR_XIR_BAD_STRUCTURE;
    bool supported=true;
    for (uint32_t p=0;p<site->captures;++p) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        XrXirType type=target->parameters[p];
        if (xr_xir_type_span(owner->module->types,type)) supported=false;
        else if (xr_xir_type_is_cell(owner->module->types,type)) {
            if (xr_xir_cell_provenance_role(owner->effects->cells,site->target,p)!=
                XR_XIR_CELL_PROOF_OWNED_CAPTURE) supported=false;
        } else if (xr_xir_type_node(owner->module->types,type)) supported=false;
    }
    *output=supported;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_producer_atoms(const EffectInvocationOwner *owner,
    const EffectInvocationBasis *basis,uint32_t *output) {
    if (!owner || !basis || !output) return XR_XIR_BAD_STRUCTURE;
    uint64_t bits=(uint64_t)basis->parameters+3;
    if (bits>UINT32_MAX) return XR_XIR_BUDGET;
    uint64_t atoms=(uint64_t)owner->binding_count*bits+1;
    if (atoms>UINT32_MAX) return XR_XIR_BUDGET;
    if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
    *output=(uint32_t)atoms;return XR_XIR_OK;
}

/* The code names one real capture column of this original site. Different
 * sites cannot read each other's flat binding or synthesize an owned role. */
static XrXirStatus effect_invocation_producer_column(const EffectInvocationOwner *owner,
    const EffectInvocationBasis *basis,uint32_t index,uint32_t atom,uint32_t *word,uint32_t *bit) {
    if (!owner || !basis || !atom || !word || !bit || index>=owner->site_count)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t bits=(uint64_t)basis->parameters+3;
    uint64_t binding=((uint64_t)atom-1)/bits,cell_bit=((uint64_t)atom-1)%bits;
    const EffectInvocationSite *site=&owner->sites[index];
    if (!xir_compile_work(owner->work,5)) return XR_XIR_BUDGET;
    if (site->target>=owner->module->function_count || binding>=owner->binding_count ||
        binding<site->binding || binding-site->binding>=site->captures) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *target=&owner->module->functions[site->target];
    uint64_t capture=binding-site->binding;
    if (capture>=target->parameter_count ||
        !xr_xir_type_is_cell(owner->module->types,target->parameters[capture])) return XR_XIR_BAD_STRUCTURE;
    uint64_t column=basis->origin_words+binding*((uint64_t)basis->origin_words+basis->cell_words)+
        basis->origin_words+cell_bit/64;
    if (column>=basis->fn_words) return XR_XIR_BAD_STRUCTURE;
    *word=(uint32_t)column;*bit=(uint32_t)(cell_bit%64);return XR_XIR_OK;
}

static const EffectInvocationProducer *effect_invocation_producer_find_atom(
    const EffectInvocationNode *node,uint32_t value,uint32_t site,uint32_t atom) {
    for (uint32_t p=0;p<node->producer_count;++p)
        if (node->producers[p].value==value && node->producers[p].site==site &&
            node->producers[p].atom==atom) return &node->producers[p];
    return NULL;
}
static inline const EffectInvocationProducer *effect_invocation_producer_find(
    const EffectInvocationNode *node,uint32_t value,uint32_t site) {
    return effect_invocation_producer_find_atom(node,value,site,0);
}

static bool effect_invocation_producer_before(const EffectInvocationProducer *a,
    const EffectInvocationProducer *b) {
    if (a->distance!=b->distance) return a->distance<b->distance;
    if (a->next_node!=b->next_node) return a->next_node<b->next_node;
    if (a->next_value!=b->next_value) return a->next_value<b->next_value;
    if (a->edge!=b->edge) return a->edge<b->edge;
    return a->instruction<b->instruction;
}

/* Immutable input keys bound the facts that may enter a parameter. A
 * historical less precise key cannot acquire a site merely because a later
 * actual environment contains it; that actual must name another full key. */
static XrXirStatus effect_invocation_producer_put(EffectInvocationFlow *flow,
    const EffectInvocationProducer *fact,bool *changed,bool verify) {
    EffectInvocationOwner *owner=flow->owner;
    if (!changed || !fact || fact->value>flow->values || fact->site>=owner->site_count ||
        fact->distance==UINT64_MAX) return XR_XIR_BAD_STRUCTURE;
    bool supported=false;uint32_t atoms=0;
    XrXirStatus admitted=effect_invocation_producer_site_supported(owner,fact->site,&supported);
    if (admitted==XR_XIR_OK) admitted=effect_invocation_producer_atoms(owner,&flow->basis,&atoms);
    if (admitted!=XR_XIR_OK) return admitted;
    if (!supported || fact->atom>=atoms) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationNode *node=&owner->nodes[flow->node];
    if (fact->value<flow->values) {
        const uint64_t *row=NULL;
        if (flow->rows) row=flow->rows+(size_t)fact->value*flow->basis.fn_words;
        else if (fact->value<flow->function->parameter_count)
            row=node->input+(size_t)fact->value*flow->basis.row_words;
        if (!row) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (!(row[fact->site/64]&(UINT64_C(1)<<(fact->site%64)))) return XR_XIR_OK;
        if (fact->atom) {
            uint32_t word=0,bit=0;
            admitted=effect_invocation_producer_column(owner,&flow->basis,fact->site,fact->atom,&word,&bit);
            if (admitted!=XR_XIR_OK) return admitted;
            if (!(row[word]&(UINT64_C(1)<<bit))) return XR_XIR_OK;
        }
    }
    for (uint32_t p=0;p<node->producer_count;++p) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        EffectInvocationProducer *present=&node->producers[p];
        if (present->value!=fact->value || present->site!=fact->site || present->atom!=fact->atom) continue;
        if (verify) return present->distance<=fact->distance?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
        if (!effect_invocation_producer_before(fact,present)) return XR_XIR_OK;
        if (!xir_compile_work(owner->work,sizeof(*fact))) return XR_XIR_BUDGET;
        *present=*fact;*changed=true;return effect_invocation_enqueue(owner,flow->node);
    }
    if (verify) return XR_XIR_BAD_STRUCTURE;
    if (node->producer_count==node->producer_capacity) {
        uint64_t maximum=((uint64_t)flow->values+1)*owner->site_count;
        maximum=maximum>UINT32_MAX/atoms?UINT32_MAX:maximum*atoms;
        uint32_t capacity=node->producer_capacity?node->producer_capacity:8;
        if (capacity>maximum) capacity=(uint32_t)maximum;
        else if (node->producer_capacity) capacity=capacity>maximum/2?(uint32_t)maximum:capacity*2;
        if (capacity<=node->producer_count || (uint64_t)capacity>SIZE_MAX/sizeof(*fact)) return XR_XIR_BUDGET;
        XrXirStatus status=XR_XIR_OK;
        EffectInvocationProducer *records=xir_compile_alloc(owner->work,(size_t)capacity*sizeof(*records),&status);
        if (!records) return status;
        if (!xir_compile_work(owner->work,(uint64_t)node->producer_count*sizeof(*records))) {
            xr_compile_resources_free(records);return XR_XIR_BUDGET;
        }
        if (node->producer_count) memcpy(records,node->producers,(size_t)node->producer_count*sizeof(*records));
        xr_compile_resources_free(node->producers);node->producers=records;node->producer_capacity=capacity;
    }
    if (!xir_compile_work(owner->work,sizeof(*fact)+1)) return XR_XIR_BUDGET;
    node->producers[node->producer_count++]=*fact;*changed=true;
    return effect_invocation_enqueue(owner,flow->node);
}

static XrXirStatus effect_invocation_producer_alias(EffectInvocationFlow *flow,
    uint32_t destination,uint32_t source,uint32_t instruction,bool *changed,bool verify) {
    if (destination>=flow->values || source>=flow->values) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationNode *node=&flow->owner->nodes[flow->node];
    for (uint32_t p=0;p<node->producer_count;++p) {
        if (!xir_compile_work(flow->owner->work,2)) return XR_XIR_BUDGET;
        EffectInvocationProducer from=node->producers[p];
        if (from.value!=source) continue;
        EffectInvocationProducer fact={destination,from.site,flow->node,source,UINT32_MAX,instruction,from.distance+1,from.atom};
        XrXirStatus status=effect_invocation_producer_put(flow,&fact,changed,verify);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_producer_seed(EffectInvocationFlow *flow,
    uint32_t instruction,bool *changed,bool verify) {
    uint32_t destination=flow->function->parameter_count+instruction;
    if (!flow->certified[destination]) return XR_XIR_OK;
    uint32_t site=UINT32_MAX;XrXirStatus status=effect_invocation_site(flow,instruction,&site);
    bool supported=false;
    if (status==XR_XIR_OK) status=effect_invocation_producer_site_supported(flow->owner,site,&supported);
    if (status!=XR_XIR_OK || !supported) return status;
    EffectInvocationProducer fact={destination,site,UINT32_MAX,UINT32_MAX,UINT32_MAX,instruction,0,0};
    status=effect_invocation_producer_put(flow,&fact,changed,verify);
    const EffectInvocationSite *source=&flow->owner->sites[site];
    const XrXirFunction *target=&flow->owner->module->functions[source->target];
    const uint64_t *row=flow->rows+(size_t)destination*flow->basis.fn_words;
    uint64_t bits=(uint64_t)flow->basis.parameters+3;
    for (uint32_t p=0;status==XR_XIR_OK && p<source->captures;++p) {
        if (!xir_compile_work(flow->owner->work,2)) return XR_XIR_BUDGET;
        if (!xr_xir_type_is_cell(flow->owner->module->types,target->parameters[p])) continue;
        for (uint32_t b=0;status==XR_XIR_OK && b<bits;++b) {
            uint64_t code=1+((uint64_t)source->binding+p)*bits+b;
            if (code>UINT32_MAX) return XR_XIR_BUDGET;
            uint32_t word=0,bit=0;fact.atom=(uint32_t)code;
            status=effect_invocation_producer_column(flow->owner,&flow->basis,site,fact.atom,&word,&bit);
            if (status!=XR_XIR_OK) break;
            if (row[word]&(UINT64_C(1)<<bit)) status=effect_invocation_producer_put(flow,&fact,changed,verify);
        }
    }
    return status;
}

static XrXirStatus effect_invocation_producer_instruction(EffectInvocationFlow *flow,
    uint32_t instruction,bool *changed,bool verify) {
    const XrXirInstruction *op=&flow->function->instructions[instruction];
    uint32_t destination=flow->function->parameter_count+instruction;
    switch (op->op) {
    case XR_XIR_FUNCTION_REF:
        return effect_invocation_producer_seed(flow,instruction,changed,verify);
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_NEW:
    case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_READ:
        return effect_invocation_producer_alias(flow,destination,op->args[0],instruction,changed,verify);
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_WRITE:
        return effect_invocation_producer_alias(flow,op->args[0],op->args[1],instruction,changed,verify);
    case XR_XIR_PHI:
        if (!op->args[1] || op->args[1]%2 || !flow->function->operands ||
            op->args[0]>flow->function->operand_count ||
            op->args[1]>flow->function->operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a=1;a<op->args[1];a+=2) {
            XrXirStatus status=effect_invocation_producer_alias(flow,destination,
                flow->function->operands[op->args[0]+a],instruction,changed,verify);
            if (status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    case XR_XIR_RETURN: {
        bool supported=false;XrXirStatus status=effect_invocation_producer_return(flow->owner,
            flow->owner->nodes[flow->node].body,&supported);
        if (status!=XR_XIR_OK || !supported) return status;
        if (op->args[0]>=flow->values) return XR_XIR_BAD_VALUE;
        EffectInvocationNode *node=&flow->owner->nodes[flow->node];
        for (uint32_t p=0;p<node->producer_count;++p) {
            if (!xir_compile_work(flow->owner->work,2)) return XR_XIR_BUDGET;
            EffectInvocationProducer from=node->producers[p];
            if (from.value!=op->args[0]) continue;
            EffectInvocationProducer fact={flow->values,from.site,flow->node,from.value,
                UINT32_MAX,instruction,from.distance+1,from.atom};
            status=effect_invocation_producer_put(flow,&fact,changed,verify);
            if (status!=XR_XIR_OK) return status;
        }
        return XR_XIR_OK;
    }
    default:return XR_XIR_OK;
    }
}

/* A captured callable relation needs a different atom projection and is
 * deliberately absent here. Scalar and Cell prefixes still use their existing
 * authentic common proof; ordinary callable suffixes use actual SSA causes. */
static XrXirStatus effect_invocation_producer_actual(const EffectInvocationOwner *owner,
    const EffectInvocationEdge *edge,uint32_t parameter,uint32_t *output) {
    if (!owner || !edge || !output || edge->caller>=owner->count || edge->target>=owner->count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *caller=&owner->nodes[edge->caller],*target=&owner->nodes[edge->target];
    if (caller->body>=owner->module->function_count || target->body>=owner->module->function_count ||
        caller->root!=target->root) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&owner->module->functions[caller->body];
    if (edge->instruction>=function->instruction_count ||
        parameter>=owner->module->functions[target->body].parameter_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&function->instructions[edge->instruction];
    if (!xir_compile_work(owner->work,6)) return XR_XIR_BUDGET;
    uint32_t captures=0;
    if (op->op==XR_XIR_FUNCTION_REF) {
        if (!edge->latent || edge->producer>=owner->site_count ||
            owner->sites[edge->producer].function!=caller->body ||
            owner->sites[edge->producer].instruction!=edge->instruction ||
            owner->sites[edge->producer].target!=target->body || op->immediate<0 ||
            (uint64_t)op->immediate!=target->body) return XR_XIR_BAD_STRUCTURE;
        if (parameter>=op->args[1]) { *output=UINT32_MAX;return XR_XIR_OK; }
    } else if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) {
        if (edge->latent || edge->producer>=owner->site_count) return XR_XIR_BAD_STRUCTURE;
        if (owner->sites[edge->producer].target!=target->body) return XR_XIR_BAD_STRUCTURE;
        captures=owner->sites[edge->producer].captures;
        if (parameter<captures) { *output=UINT32_MAX;return XR_XIR_OK; }
    } else if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_GO ||
        op->op==XR_XIR_CLEANUP_REGISTER) {
        if (edge->producer!=UINT32_MAX || edge->latent!=(op->op==XR_XIR_GO) ||
            op->immediate<0 || (uint64_t)op->immediate!=target->body) return XR_XIR_BAD_STRUCTURE;
    } else { *output=UINT32_MAX;return XR_XIR_OK; }
    uint32_t actual=parameter-captures;
    if (actual>=op->args[1] || !function->operands || op->args[0]>function->operand_count ||
        op->args[1]>function->operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
    actual=function->operands[op->args[0]+actual];
    if ((uint64_t)actual>=(uint64_t)function->parameter_count+function->instruction_count)
        return XR_XIR_BAD_VALUE;
    *output=actual;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_producer_inputs(EffectInvocationFlow *flow,
    uint32_t edge_index,bool *changed,bool verify) {
    EffectInvocationOwner *owner=flow->owner;EffectInvocationEdge edge=owner->edges[edge_index];
    EffectInvocationNode *target=&owner->nodes[edge.target];
    const XrXirFunction *function=&owner->module->functions[target->body];
    uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
    if (values>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationFlow projection={.owner=owner,.node=edge.target,.function=function,
        .basis=flow->basis,.values=(uint32_t)values};
    for (uint32_t p=0;p<function->parameter_count;++p) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (!xr_xir_callable_signature(owner->module->types,function->parameters[p])) continue;
        uint32_t actual=UINT32_MAX;XrXirStatus status=effect_invocation_producer_actual(owner,&edge,p,&actual);
        if (status!=XR_XIR_OK) return status;
        if (actual==UINT32_MAX) continue;
        const EffectInvocationNode *caller=&owner->nodes[flow->node];
        for (uint32_t a=0;a<caller->producer_count;++a) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            EffectInvocationProducer from=caller->producers[a];
            if (from.value!=actual) continue;
            EffectInvocationProducer fact={p,from.site,flow->node,actual,edge_index,
                edge.instruction,from.distance+1,from.atom};
            status=effect_invocation_producer_put(&projection,&fact,changed,verify);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_producer_results(EffectInvocationFlow *flow,
    uint32_t edge_index,bool *changed,bool verify) {
    EffectInvocationOwner *owner=flow->owner;EffectInvocationEdge edge=owner->edges[edge_index];
    if (edge.latent) return XR_XIR_OK;
    const EffectInvocationNode *child=&owner->nodes[edge.target];
    const XrXirFunction *target=&owner->module->functions[child->body];
    uint64_t returned=(uint64_t)target->parameter_count+target->instruction_count;
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&flow->function->instructions[i];
        if (!xr_xir_callable_signature(owner->module->types,op->type)) continue;
        bool source=(op->op==XR_XIR_INVOKE_RESULT && op->immediate==(int64_t)edge.instruction) ||
            (i==edge.instruction && (op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_DEFAULT ||
            op->op==XR_XIR_CALL_INDIRECT));
        if (!source) continue;
        for (uint32_t p=0;p<child->producer_count;++p) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            EffectInvocationProducer from=child->producers[p];
            if (from.value!=returned) continue;
            EffectInvocationProducer fact={flow->function->parameter_count+i,from.site,
                edge.target,(uint32_t)returned,edge_index,i,from.distance+1,from.atom};
            XrXirStatus status=effect_invocation_producer_put(flow,&fact,changed,verify);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_producer_step(EffectInvocationFlow *flow,
    bool *changed,bool verify) {
    if (!flow || !changed || !flow->owner->producer_enabled) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationOwner *owner=flow->owner;
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        XrXirStatus status=effect_invocation_producer_instruction(flow,i,changed,verify);
        if (status!=XR_XIR_OK) return status;
    }
    for (uint32_t e=owner->nodes[flow->node].edge_head;e!=UINT32_MAX;e=owner->edges[e].next) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        if (e>=owner->edge_count || owner->edges[e].caller!=flow->node ||
            owner->edges[e].target>=owner->count ||
            owner->nodes[owner->edges[e].target].root!=owner->nodes[flow->node].root)
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=effect_invocation_producer_inputs(flow,e,changed,verify);
        if (status==XR_XIR_OK) status=effect_invocation_producer_results(flow,e,changed,verify);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* Every witness edge is tied to the original opcode and physical parameter,
 * not merely to matching function numbers or a stored advertisement. */
static XrXirStatus effect_invocation_producer_cause(EffectInvocationFlow *flow,
    const EffectInvocationProducer *fact) {
    EffectInvocationOwner *owner=flow->owner;
    if (fact->next_node>=owner->count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,8)) return XR_XIR_BUDGET;
    if (fact->edge!=UINT32_MAX) {
        if (fact->edge>=owner->edge_count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationEdge *edge=&owner->edges[fact->edge];
        if (fact->value<flow->function->parameter_count) {
            if (edge->target!=flow->node || edge->caller!=fact->next_node ||
                edge->instruction!=fact->instruction) return XR_XIR_BAD_STRUCTURE;
            uint32_t actual=UINT32_MAX;XrXirStatus status=effect_invocation_producer_actual(owner,edge,fact->value,&actual);
            return status!=XR_XIR_OK?status:actual==fact->next_value?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
        }
        if (edge->caller!=flow->node || edge->target!=fact->next_node || edge->latent ||
            fact->value>=flow->values || fact->instruction>=flow->function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        const XrXirFunction *target=&owner->module->functions[owner->nodes[fact->next_node].body];
        if (fact->next_value!=(uint64_t)target->parameter_count+target->instruction_count ||
            fact->value!=flow->function->parameter_count+fact->instruction) return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *op=&flow->function->instructions[fact->instruction];
        return ((op->op==XR_XIR_INVOKE_RESULT && op->immediate==(int64_t)edge->instruction) ||
            (fact->instruction==edge->instruction && (op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_DEFAULT ||
             op->op==XR_XIR_CALL_INDIRECT)))?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    if (fact->next_node!=flow->node || fact->next_value>=flow->values ||
        fact->instruction>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&flow->function->instructions[fact->instruction];
    if (fact->value==flow->values) {
        bool supported=false;XrXirStatus status=effect_invocation_producer_return(owner,
            owner->nodes[flow->node].body,&supported);
        return status!=XR_XIR_OK?status:supported && op->op==XR_XIR_RETURN &&
            op->args[0]==fact->next_value?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    uint32_t destination=flow->function->parameter_count+fact->instruction;
    switch (op->op) {
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_NEW:
    case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_READ:
        return fact->value==destination && fact->next_value==op->args[0]?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_WRITE:
        return fact->value==op->args[0] && fact->next_value==op->args[1]?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    case XR_XIR_PHI:
        if (fact->value!=destination) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a=1;a<op->args[1];a+=2) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            if (flow->function->operands[op->args[0]+a]==fact->next_value) return XR_XIR_OK;
        }
        return XR_XIR_BAD_STRUCTURE;
    default:return XR_XIR_BAD_STRUCTURE;
    }
}

static XrXirStatus effect_invocation_producer_record_verify(EffectInvocationFlow *flow,
    const EffectInvocationProducer *fact,uint64_t maximum) {
    EffectInvocationOwner *owner=flow->owner;
    if (fact->value>flow->values || fact->site>=owner->site_count ||
        fact->distance>=maximum) return XR_XIR_BAD_STRUCTURE;
    bool supported=false;uint32_t atoms=0;
    XrXirStatus admitted=effect_invocation_producer_site_supported(owner,fact->site,&supported);
    if (admitted==XR_XIR_OK) admitted=effect_invocation_producer_atoms(owner,&flow->basis,&atoms);
    if (admitted!=XR_XIR_OK) return admitted;
    if (!supported || fact->atom>=atoms) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,5)) return XR_XIR_BUDGET;
    if (fact->value<flow->values && !(flow->rows[(size_t)fact->value*flow->basis.fn_words+fact->site/64]&
        (UINT64_C(1)<<(fact->site%64)))) return XR_XIR_BAD_STRUCTURE;
    if (fact->atom && fact->value<flow->values) {
        uint32_t word=0,bit=0;
        admitted=effect_invocation_producer_column(owner,&flow->basis,fact->site,fact->atom,&word,&bit);
        if (admitted!=XR_XIR_OK) return admitted;
        if (!(flow->rows[(size_t)fact->value*flow->basis.fn_words+word]&(UINT64_C(1)<<bit)))
            return XR_XIR_BAD_STRUCTURE;
    }
    if (!fact->distance) {
        EffectInvocationSite site=owner->sites[fact->site];
        return site.function==owner->nodes[flow->node].body &&
            site.instruction==fact->instruction && fact->instruction<flow->function->instruction_count &&
            fact->value==flow->function->parameter_count+fact->instruction && flow->certified[fact->value] &&
            fact->next_node==UINT32_MAX && fact->next_value==UINT32_MAX && fact->edge==UINT32_MAX?
            XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    XrXirStatus status=effect_invocation_producer_cause(flow,fact);
    if (status!=XR_XIR_OK) return status;
    const EffectInvocationNode *next=&owner->nodes[fact->next_node];
    if (!xir_compile_work(owner->work,(uint64_t)next->producer_count*2)) return XR_XIR_BUDGET;
    const EffectInvocationProducer *cause=effect_invocation_producer_find_atom(next,fact->next_value,fact->site,fact->atom);
    return cause && cause->distance+1==fact->distance?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

/* No ROOT result is changed during this replay. Inflation, an unrooted cycle,
 * missing transfers and forged actual projections fail before owner transfer.
 * The descending chain is private and never enters the public function forest. */
static XrXirStatus effect_invocation_producers_verify(EffectInvocationOwner *owner) {
    if (!owner || !owner->work || !owner->module || owner->pending) return XR_XIR_BAD_STRUCTURE;
    uint64_t maximum=0;bool enabled=false;
    for (uint32_t f=0;f<owner->module->function_count;++f) {
        bool supported=false;XrXirStatus status=effect_invocation_producer_return(owner,f,&supported);
        if (status!=XR_XIR_OK) return status;
        enabled|=supported;
    }
    if (enabled!=owner->producer_enabled) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t n=0;n<owner->count;++n) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&owner->nodes[n];
        if (node->body>=owner->module->function_count || node->producer_count>node->producer_capacity ||
            (!!node->producers!=!!node->producer_capacity) || (!enabled && node->producer_count))
            return XR_XIR_BAD_STRUCTURE;
        const XrXirFunction *function=&owner->module->functions[node->body];
        uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
        if (values>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        EffectInvocationBasis basis={0};uint32_t atoms=0;
        XrXirStatus status=effect_invocation_basis(owner,node->root,&basis);
        if (status==XR_XIR_OK) status=effect_invocation_producer_atoms(owner,&basis,&atoms);
        if (status!=XR_XIR_OK) return status;
        uint64_t keys=(values+1)*owner->site_count;
        keys=keys>UINT32_MAX/atoms?UINT32_MAX:keys*atoms;
        if (node->producer_capacity>keys || maximum>UINT64_MAX-node->producer_count)
            return XR_XIR_BAD_STRUCTURE;
        /* Validate coordinates before origin projection can consume a record.
         * A real Cell environment still enters the complete charged replay. */
        for (uint32_t p=0;p<node->producer_count;++p) {
            if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
            const EffectInvocationProducer *fact=&node->producers[p];
            if (fact->value>values || fact->site>=owner->site_count || fact->atom>=atoms)
                return XR_XIR_BAD_STRUCTURE;
            bool supported=false;
            status=effect_invocation_producer_site_supported(owner,fact->site,&supported);
            if (status!=XR_XIR_OK) return status;
            if (!supported) return XR_XIR_BAD_STRUCTURE;
            if (fact->atom) {
                uint32_t word=0,bit=0;
                status=effect_invocation_producer_column(owner,&basis,fact->site,fact->atom,&word,&bit);
                if (status!=XR_XIR_OK) return status;
            }
        }
        maximum+=node->producer_count;
    }
    if (!enabled) return XR_XIR_OK;
    for (uint32_t n=0;n<owner->count;++n) {
        EffectInvocationFlow flow={.owner=owner,.node=n,.instruction=UINT32_MAX};
        XrXirStatus status=effect_invocation_origins(&flow);
        const EffectInvocationNode *node=&owner->nodes[n];
        for (uint32_t p=0;status==XR_XIR_OK && p<node->producer_count;++p) {
            const EffectInvocationProducer *fact=&node->producers[p];
            for (uint32_t a=0;a<p;++a) {
                if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
                if (node->producers[a].value==fact->value && node->producers[a].site==fact->site &&
                    node->producers[a].atom==fact->atom) {
                    status=XR_XIR_BAD_STRUCTURE;break;
                }
            }
            if (status==XR_XIR_OK) status=effect_invocation_producer_record_verify(&flow,fact,maximum);
        }
        bool changed=false;
        if (status==XR_XIR_OK) status=effect_invocation_producer_step(&flow,&changed,true);
        xr_compile_resources_free(flow.memory);
        if (status!=XR_XIR_OK) return status;
        if (changed || owner->pending) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
