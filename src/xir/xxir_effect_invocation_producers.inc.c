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
static XrXirStatus effect_invocation_producer_return_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,uint32_t body,bool *output) {
    if (!owner || !request || !output || body>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionHead head={0};
    XrXirStatus status=effect_body_request_head(request,body,&head);
    if (status!=XR_XIR_OK) return status;
    const XrXirTypeNode *signature=xr_xir_callable_signature(request->domain.types,
        head.result);
    if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
    if (signature && (!xr_xir_callable_flags_valid(signature->flags) ||
        (!!signature->parameters!=!!signature->parameter_count))) return XR_XIR_BAD_TYPE;
    bool supported=signature && !signature->parameter_span &&
        (signature->flags&XR_XIR_CALLABLE_ROOT_MASK)==XR_XIR_CALLABLE_ROOT_NONE;
    for (uint32_t p=0;signature && p<signature->parameter_count;++p) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        if (!xr_xir_callable_parameter_storage_valid(request->domain.types,&signature->parameters[p]))
            return XR_XIR_BAD_TYPE;
        if (signature->parameters[p].mode!=XR_PARAM_READ) supported=false;
    }
    *output=supported;return XR_XIR_OK;
}
/* Only builtin scalar and authentic owned Cell prefixes participate. A
 * captured callable or aggregate needs its complete relation rather than an
 * invented empty environment, so no identity fact is emitted for that site. */
static XrXirStatus effect_invocation_producer_site_supported_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,uint32_t index,bool *output) {
    if (!owner || !request || !output || index>=owner->final->site_count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationSite *site=&owner->final->sites[index];
    if (site->target>=request->domain.function_count || site->function>=request->domain.function_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionHead target_head={0},producer_head={0};
    XrXirStatus status=effect_body_request_head(request,site->target,&target_head);
    if (status==XR_XIR_OK) status=effect_body_request_head(request,site->function,&producer_head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionHead *target=&target_head,*producer=&producer_head;
    if (site->captures>target->parameter_count || site->instruction>=producer->instruction_count ||
        site->binding>owner->final->binding_count || site->captures>owner->final->binding_count-site->binding)
        return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    status=effect_body_request_instruction(request,site->function,site->instruction,&storage,&op);
    if (status!=XR_XIR_OK) return status;
    if (op->op!=XR_XIR_FUNCTION_REF || op->immediate!=(int64_t)site->target || op->args[1]!=site->captures)
        return XR_XIR_BAD_STRUCTURE;
    bool supported=true;
    for (uint32_t p=0;p<site->captures;++p) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        XrXirType type=target->parameters[p];
        if (xr_xir_type_span(request->domain.types,type)) supported=false;
        else if (xr_xir_type_is_cell(request->domain.types,type)) {
            if (xr_xir_cell_provenance_role(owner->final->cells,site->target,p)!=
                XR_XIR_CELL_PROOF_OWNED_CAPTURE) supported=false;
        } else if (xr_xir_type_node(request->domain.types,type) &&
            !xr_xir_callable_signature(request->domain.types,type)) supported=false;
    }
    *output=supported;return XR_XIR_OK;
}
static inline XrXirStatus effect_invocation_producer_site_supported(const EffectInvocationWorkspace *owner,
    uint32_t index,bool *output) {
    if (!owner || !effect_body_material_shape(owner->final.material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(owner->read.work,owner->final.material);
    return effect_invocation_producer_site_supported_read(&owner->read,&request,index,output);
}

static XrXirStatus effect_invocation_producer_atoms(const EffectInvocationRead *owner,
    const EffectInvocationBasis *basis,uint32_t *output) {
    if (!owner || !basis || !output) return XR_XIR_BAD_STRUCTURE;
    uint64_t bits=(uint64_t)basis->parameters+3;
    if (bits>UINT32_MAX) return XR_XIR_BUDGET;
    if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
    uint64_t stride=(uint64_t)owner->final->site_count+2*bits;
    if (owner->final->binding_count && stride>((uint64_t)UINT32_MAX-1)/owner->final->binding_count)
        return XR_XIR_BUDGET;
    uint64_t atoms=(uint64_t)owner->final->binding_count*stride+1;
    if (atoms>UINT32_MAX) return XR_XIR_BUDGET;
    if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
    *output=(uint32_t)atoms;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_producer_binding_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,uint32_t binding,XrXirType *output) {
    if(binding>=owner->final->binding_count) return XR_XIR_BAD_STRUCTURE;
    for(uint32_t s=0;s<owner->final->site_count;++s) {
        if(!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        const EffectInvocationSite *site=&owner->final->sites[s];
        if(binding<site->binding || binding-site->binding>=site->captures) continue;
        if(site->target>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
        EffectBodyFunctionHead target={0};XrXirStatus status=effect_body_request_head(request,site->target,&target);
        if(status!=XR_XIR_OK) return status;
        uint32_t capture=binding-site->binding;
        if(capture>=target.parameter_count) return XR_XIR_BAD_STRUCTURE;
        *output=target.parameters[capture];return XR_XIR_OK;
    }
    return XR_XIR_BAD_STRUCTURE;
}

/* Each binding has disjoint Cell and callable-origin coordinates. Nested
 * closures retain columns belonging to inner sites too. The actual binding
 * owner supplies the type; projection alone proves neither presence nor a
 * source. The complete finite relation checks those separate obligations. */
static XrXirStatus effect_invocation_producer_column_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,const EffectInvocationBasis *basis,
    uint32_t index,uint32_t atom,EffectInvocationColumn *output) {
    if (!owner || !request || !basis || !atom || !output || index>=owner->final->site_count)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,6)) return XR_XIR_BUDGET;
    uint64_t bits=(uint64_t)basis->parameters+3;
    uint64_t stride=(uint64_t)owner->final->site_count+2*bits;
    uint64_t binding=((uint64_t)atom-1)/stride,selected_bit=((uint64_t)atom-1)%stride;
    if (!xir_compile_work(owner->work,5)) return XR_XIR_BUDGET;
    if(binding>=owner->final->binding_count) return XR_XIR_BAD_STRUCTURE;
    XrXirType type=0;XrXirStatus status=effect_invocation_producer_binding_read(owner,request,(uint32_t)binding,&type);
    if(status!=XR_XIR_OK) return status;
    bool callable=selected_bit>=bits;
    uint64_t bit=callable?selected_bit-bits:selected_bit;
    if (callable?!xr_xir_callable_signature(request->domain.types,type):
        !xr_xir_type_is_cell(request->domain.types,type)) return XR_XIR_BAD_STRUCTURE;
    uint64_t column=basis->origin_words+binding*((uint64_t)basis->origin_words+basis->cell_words)+
        (callable?0:basis->origin_words)+bit/64;
    if (column>=basis->fn_words) return XR_XIR_BAD_STRUCTURE;
    *output=(EffectInvocationColumn){(uint32_t)column,(uint32_t)(bit%64)};return XR_XIR_OK;
}

static inline XrXirStatus effect_invocation_producer_column(const EffectInvocationWorkspace *owner,
    const EffectInvocationBasis *basis,uint32_t index,uint32_t atom,uint32_t *word,uint32_t *bit) {
    if (!owner || !effect_body_material_shape(owner->final.material) || !word || !bit) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(owner->read.work,owner->final.material);
    EffectInvocationColumn column={0};
    XrXirStatus status=effect_invocation_producer_column_read(&owner->read,&request,basis,index,atom,&column);
    if (status==XR_XIR_OK) { *word=column.word;*bit=column.bit; }
    return status;
}

/* One local view publishes both column coordinates only after the same
 * complete typed site projection succeeds. It owns no proof or cache. */
static XrXirStatus effect_invocation_producer_column_flow(EffectInvocationFlow *flow,
    uint32_t site,uint32_t atom,uint32_t *word,uint32_t *bit) {
    if (!word || !bit) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationColumn column={0};
    XrXirStatus status=effect_invocation_producer_column_read(flow->read,&flow->body,
        &flow->basis,site,atom,&column);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(flow->read->work,sizeof(*word)+sizeof(*bit))) return XR_XIR_BUDGET;
    *word=column.word;*bit=column.bit;return XR_XIR_OK;
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
    const EffectInvocationRead *owner=flow->read;
    if (!changed || !fact || fact->value>flow->values || fact->site>=owner->final->site_count ||
        fact->distance==UINT64_MAX) return XR_XIR_BAD_STRUCTURE;
    bool supported=false;uint32_t atoms=0;
    XrXirStatus admitted=effect_invocation_producer_site_supported_read(owner,&flow->body,fact->site,&supported);
    if (admitted==XR_XIR_OK) admitted=effect_invocation_producer_atoms(owner,&flow->basis,&atoms);
    if (admitted!=XR_XIR_OK) return admitted;
    if (!supported || fact->atom>=atoms) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&owner->final->nodes[flow->node];
    if (fact->value<flow->values) {
        const uint64_t *row=NULL;
        if (flow->memory) row=effect_invocation_row(flow,fact->value);
        else if (fact->value<flow->function->parameter_count)
            row=node->input+(size_t)fact->value*flow->basis.row_words;
        if (!row) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (!(row[fact->site/64]&(UINT64_C(1)<<(fact->site%64)))) return XR_XIR_OK;
        if (fact->atom) {
            uint32_t word=0,bit=0;
            admitted=effect_invocation_producer_column_flow(flow,fact->site,fact->atom,&word,&bit);
            if (admitted!=XR_XIR_OK) return admitted;
            if (!(row[word]&(UINT64_C(1)<<bit))) return XR_XIR_OK;
        }
    }
    for (uint32_t p=0;p<node->producer_count;++p) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        const EffectInvocationProducer *present=&node->producers[p];
        if (present->value!=fact->value || present->site!=fact->site || present->atom!=fact->atom) continue;
        if (verify) return present->distance<=fact->distance?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
        if (!effect_invocation_producer_before(fact,present)) return XR_XIR_OK;
        if (!xir_compile_work(owner->work,sizeof(*fact))) return XR_XIR_BUDGET;
        if (!flow->owner || owner!=&flow->owner->read) return XR_XIR_BAD_STRUCTURE;
        flow->owner->final.nodes[flow->node].producers[p]=*fact;
        *changed=true;return effect_invocation_enqueue(flow->owner,flow->node);
    }
    if (verify) return XR_XIR_BAD_STRUCTURE;
    if (!flow->owner || owner!=&flow->owner->read) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationNode *receiving=&flow->owner->final.nodes[flow->node];
    if (receiving->producer_count==receiving->producer_capacity) {
        uint64_t maximum=((uint64_t)flow->values+1)*owner->final->site_count;
        maximum=maximum>UINT32_MAX/atoms?UINT32_MAX:maximum*atoms;
        uint32_t capacity=receiving->producer_capacity?receiving->producer_capacity:8;
        if (capacity>maximum) capacity=(uint32_t)maximum;
        else if (receiving->producer_capacity) capacity=capacity>maximum/2?(uint32_t)maximum:capacity*2;
        if (capacity<=receiving->producer_count || (uint64_t)capacity>SIZE_MAX/sizeof(*fact)) return XR_XIR_BUDGET;
        XrXirStatus status=XR_XIR_OK;
        EffectInvocationProducer *records=xir_compile_alloc(owner->work,(size_t)capacity*sizeof(*records),&status);
        if (!records) return status;
        if (!xir_compile_work(owner->work,(uint64_t)receiving->producer_count*sizeof(*records))) {
            xr_compile_resources_free(records);return XR_XIR_BUDGET;
        }
        if (receiving->producer_count) memcpy(records,receiving->producers,(size_t)receiving->producer_count*sizeof(*records));
        xr_compile_resources_free(receiving->producers);receiving->producers=records;receiving->producer_capacity=capacity;
    }
    if (!xir_compile_work(owner->work,sizeof(*fact)+1)) return XR_XIR_BUDGET;
    receiving->producers[receiving->producer_count++]=*fact;*changed=true;
    return effect_invocation_enqueue(flow->owner,flow->node);
}

static XrXirStatus effect_invocation_producer_alias(EffectInvocationFlow *flow,
    uint32_t destination,uint32_t source,uint32_t instruction,bool *changed,bool verify) {
    if (destination>=flow->values || source>=flow->values) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationNode *node=&flow->read->final->nodes[flow->node];
    for (uint32_t p=0;p<node->producer_count;++p) {
        if (!xir_compile_work(flow->read->work,2)) return XR_XIR_BUDGET;
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
    bool certified=false;XrXirStatus status=effect_invocation_flag_get(flow,destination,true,&certified);
    if (status!=XR_XIR_OK || !certified) return status;
    uint32_t site=UINT32_MAX;status=effect_invocation_site(flow,instruction,&site);
    bool supported=false;
    if (status==XR_XIR_OK) status=effect_invocation_producer_site_supported_read(flow->read,&flow->body,site,&supported);
    if (status!=XR_XIR_OK || !supported) return status;
    EffectInvocationProducer fact={destination,site,UINT32_MAX,UINT32_MAX,UINT32_MAX,instruction,0,0};
    status=effect_invocation_producer_put(flow,&fact,changed,verify);
    if (status!=XR_XIR_OK) return status;
    /* The first insertion can allocate. Reopen the actual target before
     * reading its environment, without retaining a head across later puts. */
    {
        EffectInvocationSite source=flow->read->final->sites[site];EffectBodyFunctionHead target={0};
        status=effect_invocation_flow_head(flow,source.target,&target);if(status!=XR_XIR_OK) return status;
        if(source.captures>target.parameter_count) return XR_XIR_BAD_STRUCTURE;
    }
    const uint64_t *row=effect_invocation_row(flow,destination);
    if (!row) return XR_XIR_BAD_STRUCTURE;
    uint64_t bits=(uint64_t)flow->basis.parameters+3;
    uint64_t stride=(uint64_t)flow->read->final->site_count+2*bits;
    uint64_t binding_words=(uint64_t)flow->basis.origin_words+flow->basis.cell_words;
    for(uint32_t binding=0;status==XR_XIR_OK && binding<flow->read->final->binding_count;++binding) {
        uint64_t first=flow->basis.origin_words+(uint64_t)binding*binding_words;bool present=false;
        if(first>flow->basis.fn_words || binding_words>flow->basis.fn_words-first) return XR_XIR_BAD_STRUCTURE;
        for(uint64_t w=0;w<binding_words;++w) {
            if(!xir_compile_work(flow->read->work,2)) return XR_XIR_BUDGET;
            if(row[first+w]) present=true;
        }
        if(!present) continue;
        XrXirType type=0;status=effect_invocation_producer_binding_read(flow->read,&flow->body,binding,&type);
        if(status!=XR_XIR_OK) return status;
        bool callable=xr_xir_callable_signature(flow->body.domain.types,type)!=NULL;
        bool cell=xr_xir_type_is_cell(flow->body.domain.types,type);
        if(!callable && !cell) continue;
        uint64_t count=callable?(uint64_t)flow->read->final->site_count+bits:bits;
        uint64_t words=callable?flow->basis.origin_words:flow->basis.cell_words;
        first+=callable?0:flow->basis.origin_words;
        for(uint64_t w=0;status==XR_XIR_OK && w<words;++w) {
            if(!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
            uint64_t origins=row[first+w];if(!origins) continue;
            if(w*64>=count) return XR_XIR_BAD_STRUCTURE;
            uint32_t valid=(uint32_t)(count-w*64<64?count-w*64:64);
            if(valid<64 && (origins>>valid)) return XR_XIR_BAD_STRUCTURE;
            for(uint32_t b=0;b<valid;++b) {
                if(!xir_compile_work(flow->read->work,2)) return XR_XIR_BUDGET;
                if(!(origins&(UINT64_C(1)<<b))) continue;
                uint64_t code=1+(uint64_t)binding*stride+(callable?bits:0)+w*64+b;
                if(code>UINT32_MAX) return XR_XIR_BUDGET;
                fact.atom=(uint32_t)code;status=effect_invocation_producer_put(flow,&fact,changed,verify);
                if(status!=XR_XIR_OK) break;
            }
        }
    }
    return status;
}

static XrXirStatus effect_invocation_producer_instruction(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *op,bool *changed,bool verify) {
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
        bool supported=false;XrXirStatus status=effect_invocation_producer_return_read(flow->read,&flow->body,
            flow->read->final->nodes[flow->node].body,&supported);
        if (status!=XR_XIR_OK || !supported) return status;
        if (op->args[0]>=flow->values) return XR_XIR_BAD_VALUE;
        EffectInvocationNode *node=&flow->read->final->nodes[flow->node];
        for (uint32_t p=0;p<node->producer_count;++p) {
            if (!xir_compile_work(flow->read->work,2)) return XR_XIR_BUDGET;
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
static XrXirStatus effect_invocation_producer_actual_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,const EffectInvocationEdge *edge,uint32_t parameter,uint32_t *output) {
    if (!owner || !request || !edge || !output || edge->caller>=owner->final->count || edge->target>=owner->final->count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *caller=&owner->final->nodes[edge->caller],*target=&owner->final->nodes[edge->target];
    if (caller->body>=request->domain.function_count || target->body>=request->domain.function_count ||
        caller->root!=target->root) return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionHead caller_head={0},target_head={0};
    XrXirStatus status=effect_body_request_head(request,caller->body,&caller_head);
    if (status==XR_XIR_OK) status=effect_body_request_head(request,target->body,&target_head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionHead *function=&caller_head;
    if (edge->instruction>=function->instruction_count ||
        parameter>=target_head.parameter_count) return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    status=effect_body_request_instruction(request,caller->body,edge->instruction,&storage,&op);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(owner->work,6)) return XR_XIR_BUDGET;
    uint32_t captures=0;
    if (op->op==XR_XIR_FUNCTION_REF) {
        if (!edge->latent || edge->producer>=owner->final->site_count ||
            owner->final->sites[edge->producer].function!=caller->body ||
            owner->final->sites[edge->producer].instruction!=edge->instruction ||
            owner->final->sites[edge->producer].target!=target->body || op->immediate<0 ||
            (uint64_t)op->immediate!=target->body) return XR_XIR_BAD_STRUCTURE;
        if (parameter>=op->args[1]) { *output=UINT32_MAX;return XR_XIR_OK; }
    } else if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) {
        if (edge->latent || edge->producer>=owner->final->site_count) return XR_XIR_BAD_STRUCTURE;
        if (owner->final->sites[edge->producer].target!=target->body) return XR_XIR_BAD_STRUCTURE;
        captures=owner->final->sites[edge->producer].captures;
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
static inline XrXirStatus effect_invocation_producer_actual(const EffectInvocationWorkspace *owner,
    const EffectInvocationEdge *edge,uint32_t parameter,uint32_t *output) {
    if (!owner || !effect_body_material_shape(owner->final.material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(owner->read.work,owner->final.material);
    return effect_invocation_producer_actual_read(&owner->read,&request,edge,parameter,output);
}

static XrXirStatus effect_invocation_producer_inputs(EffectInvocationFlow *flow,
    uint32_t edge_index,bool *changed,bool verify) {
    const EffectInvocationRead *owner=flow->read;EffectInvocationEdge edge=owner->final->edges[edge_index];
    EffectInvocationNode *target=&owner->final->nodes[edge.target];
    EffectBodyFunctionHead target_head={0};
    XrXirStatus read_status=effect_invocation_flow_head(flow,target->body,&target_head);
    if (read_status!=XR_XIR_OK) return read_status;
    const EffectBodyFunctionHead *function=&target_head;
    uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
    if (values>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,sizeof(flow->body))) return XR_XIR_BUDGET;
    EffectInvocationFlow projection={.owner=flow->owner,.read=owner,.node=edge.target,.function=function,
        .body=flow->body,.basis=flow->basis,.values=(uint32_t)values};
    for (uint32_t p=0;p<function->parameter_count;++p) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (!xr_xir_callable_signature(flow->body.domain.types,function->parameters[p])) continue;
        uint32_t actual=UINT32_MAX;XrXirStatus status=effect_invocation_producer_actual_read(owner,&flow->body,&edge,p,&actual);
        if (status!=XR_XIR_OK) return status;
        if (actual==UINT32_MAX) continue;
        const EffectInvocationNode *caller=&owner->final->nodes[flow->node];
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
    const EffectInvocationRead *owner=flow->read;EffectInvocationEdge edge=owner->final->edges[edge_index];
    if (edge.latent) return XR_XIR_OK;
    const EffectInvocationNode *child=&owner->final->nodes[edge.target];
    EffectBodyFunctionHead target_head={0};
    XrXirStatus read_status=effect_invocation_flow_head(flow,child->body,&target_head);
    if (read_status!=XR_XIR_OK) return read_status;
    const EffectBodyFunctionHead *target=&target_head;
    uint64_t returned=(uint64_t)target->parameter_count+target->instruction_count;
    EffectBodyRecordCursor cursor;
    read_status=effect_body_request_sequence_begin(&flow->body,&cursor);
    if (read_status!=XR_XIR_OK) return read_status;
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        XrXirInstruction body_op_2;const XrXirInstruction *op=NULL;
        XrXirStatus body_read_2=effect_body_request_sequence_instruction(&flow->body,
            owner->final->nodes[flow->node].body,i,&cursor,&body_op_2,&op);
        if (body_read_2!=XR_XIR_OK) return body_read_2;
        if (!xr_xir_callable_signature(flow->body.domain.types,op->type)) continue;
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
    return effect_body_request_sequence_finish(&flow->body,owner->final->nodes[flow->node].body,&cursor);
}

static XrXirStatus effect_invocation_producer_step(EffectInvocationFlow *flow,
    bool *changed,bool verify) {
    if (!flow || !changed || !flow->read->producer_enabled) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationRead *owner=flow->read;EffectBodyRecordCursor cursor;
    XrXirStatus read_status=effect_body_request_sequence_begin(&flow->body,&cursor);
    if (read_status!=XR_XIR_OK) return read_status;
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        XrXirStatus status=effect_body_request_sequence_instruction(&flow->body,
            owner->final->nodes[flow->node].body,i,&cursor,&storage,&op);
        if (status==XR_XIR_OK) status=effect_invocation_producer_instruction(flow,i,op,changed,verify);
        if (status!=XR_XIR_OK) return status;
    }
    read_status=effect_body_request_sequence_finish(&flow->body,owner->final->nodes[flow->node].body,&cursor);
    if (read_status!=XR_XIR_OK) return read_status;
    for (uint32_t e=owner->final->nodes[flow->node].edge_head;e!=UINT32_MAX;e=owner->final->edges[e].next) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        if (e>=owner->final->edge_count || owner->final->edges[e].caller!=flow->node ||
            owner->final->edges[e].target>=owner->final->count ||
            owner->final->nodes[owner->final->edges[e].target].root!=owner->final->nodes[flow->node].root)
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=effect_invocation_producer_inputs(flow,e,changed,verify);
        if (status==XR_XIR_OK) status=effect_invocation_producer_results(flow,e,changed,verify);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* Every witness edge is tied to the original opcode and physical parameter,
 * not merely to matching function numbers or a stored advertisement. */
static XrXirStatus effect_invocation_producer_cause_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *body,uint32_t selected,const EffectBodyFunctionHead *head,
    const EffectInvocationProducer *fact) {
    uint64_t count=(uint64_t)head->parameter_count+head->instruction_count;
    if(count>UINT32_MAX || selected>=owner->final->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t values=(uint32_t)count;
    if (fact->next_node>=owner->final->count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,8)) return XR_XIR_BUDGET;
    if (fact->edge!=UINT32_MAX) {
        if (fact->edge>=owner->final->edge_count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationEdge *edge=&owner->final->edges[fact->edge];
        if (fact->value<head->parameter_count) {
            if (edge->target!=selected || edge->caller!=fact->next_node ||
                edge->instruction!=fact->instruction) return XR_XIR_BAD_STRUCTURE;
            uint32_t actual=UINT32_MAX;XrXirStatus status=effect_invocation_producer_actual_read(owner,body,edge,fact->value,&actual);
            return status!=XR_XIR_OK?status:actual==fact->next_value?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
        }
        if (edge->caller!=selected || edge->target!=fact->next_node || edge->latent ||
            fact->value>=values || fact->instruction>=head->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        EffectBodyFunctionHead target_head={0};
        XrXirStatus head_status=effect_body_request_head(body,owner->final->nodes[fact->next_node].body,&target_head);
        if (head_status!=XR_XIR_OK) return head_status;
        const EffectBodyFunctionHead *target=&target_head;
        if (fact->next_value!=(uint64_t)target->parameter_count+target->instruction_count ||
            fact->value!=head->parameter_count+fact->instruction) return XR_XIR_BAD_STRUCTURE;
        XrXirInstruction body_op_3;const XrXirInstruction *op=NULL;
        XrXirStatus body_read_3=effect_body_request_instruction(body,owner->final->nodes[selected].body,fact->instruction,&body_op_3,&op);
        if (body_read_3!=XR_XIR_OK) return body_read_3;
        return ((op->op==XR_XIR_INVOKE_RESULT && op->immediate==(int64_t)edge->instruction) ||
            (fact->instruction==edge->instruction && (op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_DEFAULT ||
             op->op==XR_XIR_CALL_INDIRECT)))?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    if (fact->next_node!=selected || fact->next_value>=values ||
        fact->instruction>=head->instruction_count) return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction body_op_4;const XrXirInstruction *op=NULL;
    XrXirStatus body_read_4=effect_body_request_instruction(body,owner->final->nodes[selected].body,fact->instruction,&body_op_4,&op);
    if (body_read_4!=XR_XIR_OK) return body_read_4;
    if (fact->value==values) {
        bool supported=false;XrXirStatus status=effect_invocation_producer_return_read(owner,body,
            owner->final->nodes[selected].body,&supported);
        return status!=XR_XIR_OK?status:supported && op->op==XR_XIR_RETURN &&
            op->args[0]==fact->next_value?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    uint32_t destination=head->parameter_count+fact->instruction;
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
            if (head->operands[op->args[0]+a]==fact->next_value) return XR_XIR_OK;
        }
        return XR_XIR_BAD_STRUCTURE;
    default:return XR_XIR_BAD_STRUCTURE;
    }
}

static XrXirStatus effect_invocation_producer_record_verify(EffectInvocationFlow *flow,
    const EffectInvocationProducer *fact,uint64_t maximum) {
    const EffectInvocationRead *owner=flow->read;
    if (fact->value>flow->values || fact->site>=owner->final->site_count ||
        fact->distance>=maximum) return XR_XIR_BAD_STRUCTURE;
    bool supported=false;uint32_t atoms=0;
    XrXirStatus admitted=effect_invocation_producer_site_supported_read(owner,&flow->body,fact->site,&supported);
    if (admitted==XR_XIR_OK) admitted=effect_invocation_producer_atoms(owner,&flow->basis,&atoms);
    if (admitted!=XR_XIR_OK) return admitted;
    if (!supported || fact->atom>=atoms) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,5)) return XR_XIR_BUDGET;
    const uint64_t *row=fact->value<flow->values ? effect_invocation_row(flow,fact->value) : NULL;
    if (fact->value<flow->values && (!row || !(row[fact->site/64]&
        (UINT64_C(1)<<(fact->site%64))))) return XR_XIR_BAD_STRUCTURE;
    if (fact->atom && fact->value<flow->values) {
        uint32_t word=0,bit=0;
        admitted=effect_invocation_producer_column_flow(flow,fact->site,fact->atom,&word,&bit);
        if (admitted!=XR_XIR_OK) return admitted;
        if (!(row[word]&(UINT64_C(1)<<bit)))
            return XR_XIR_BAD_STRUCTURE;
    }
    if (!fact->distance) {
        bool certified=false;
        XrXirStatus flag_status=effect_invocation_flag_get(flow,fact->value,true,&certified);
        if (flag_status!=XR_XIR_OK) return flag_status;
        EffectInvocationSite site=owner->final->sites[fact->site];
        return site.function==owner->final->nodes[flow->node].body &&
            site.instruction==fact->instruction && fact->instruction<flow->function->instruction_count &&
            fact->value==flow->function->parameter_count+fact->instruction && certified &&
            fact->next_node==UINT32_MAX && fact->next_value==UINT32_MAX && fact->edge==UINT32_MAX?
            XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    XrXirStatus status=effect_invocation_producer_cause_read(flow->read,&flow->body,flow->node,flow->function,fact);
    if (status!=XR_XIR_OK) return status;
    const EffectInvocationNode *next=&owner->final->nodes[fact->next_node];
    if (!xir_compile_work(owner->work,(uint64_t)next->producer_count*2)) return XR_XIR_BUDGET;
    const EffectInvocationProducer *cause=effect_invocation_producer_find_atom(next,fact->next_value,fact->site,fact->atom);
    return cause && cause->distance+1==fact->distance?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

/* No ROOT result is changed during this replay. Inflation, an unrooted cycle,
 * missing transfers and forged actual projections fail before owner transfer.
 * The descending chain is private and never enters the public function forest. */
static XrXirStatus effect_invocation_producers_verify_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request) {
    if (!owner || !owner->work || !request || !request->owner || !request->domain.types) return XR_XIR_BAD_STRUCTURE;
    uint64_t maximum=0;bool enabled=owner->final->site_count!=0;
    if(enabled!=owner->producer_enabled) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t n=0;n<owner->final->count;++n) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&owner->final->nodes[n];
        if (node->body>=request->domain.function_count || node->producer_count>node->producer_capacity ||
            (!!node->producers!=!!node->producer_capacity) || (!enabled && node->producer_count))
            return XR_XIR_BAD_STRUCTURE;
        EffectBodyFunctionDimensions body_head={0};
        XrXirStatus head_status=effect_body_request_dimensions(request,node->body,&body_head);
        if (head_status!=XR_XIR_OK) return head_status;
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        uint32_t root_parameters=body_head.parameter_count;
        if (node->root!=node->body) {
            EffectBodyFunctionDimensions root_head={0};
            head_status=effect_body_request_dimensions(request,node->root,&root_head);
            if (head_status!=XR_XIR_OK) return head_status;
            root_parameters=root_head.parameter_count;
        }
        uint64_t values=(uint64_t)body_head.parameter_count+body_head.instruction_count;
        if (values>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        EffectInvocationBasis basis={0};uint32_t atoms=0;
        XrXirStatus status=effect_invocation_basis_parameters(owner,root_parameters,&basis);
        if (status==XR_XIR_OK) status=effect_invocation_producer_atoms(owner,&basis,&atoms);
        if (status!=XR_XIR_OK) return status;
        uint64_t keys=(values+1)*owner->final->site_count;
        keys=keys>UINT32_MAX/atoms?UINT32_MAX:keys*atoms;
        if (node->producer_capacity>keys || maximum>UINT64_MAX-node->producer_count)
            return XR_XIR_BAD_STRUCTURE;
        /* Validate coordinates before origin projection can consume a record.
         * A real Cell environment still enters the complete charged replay. */
        for (uint32_t p=0;p<node->producer_count;++p) {
            if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
            const EffectInvocationProducer *fact=&node->producers[p];
            if (fact->value>values || fact->site>=owner->final->site_count || fact->atom>=atoms)
                return XR_XIR_BAD_STRUCTURE;
            bool supported=false;
            status=effect_invocation_producer_site_supported_read(owner,request,fact->site,&supported);
            if (status!=XR_XIR_OK) return status;
            if (!supported) return XR_XIR_BAD_STRUCTURE;
            if (fact->atom) {
                EffectInvocationColumn column={0};
                status=effect_invocation_producer_column_read(owner,request,&basis,fact->site,fact->atom,&column);
                if (status!=XR_XIR_OK) return status;
            }
        }
        maximum+=node->producer_count;
    }
    if (!enabled) return XR_XIR_OK;
    for (uint32_t n=0;n<owner->final->count;++n) {
        EffectInvocationFlow flow={.read=owner,.node=n,.instruction=UINT32_MAX};
        XrXirStatus status=effect_invocation_origins_read(&flow,request);
        const EffectInvocationNode *node=&owner->final->nodes[n];
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
        if (changed) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

/* Construction supplies its already-verified input to the same replay core.
 * Published certificates instead supply only their immutable owning reader. */
static XrXirStatus effect_invocation_producers_verify(EffectInvocationWorkspace *owner) {
    if (!owner || owner->pending || !effect_body_material_shape(owner->final.material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(owner->read.work,owner->final.material);
    return effect_invocation_producers_verify_read(&owner->read,&request);
}
