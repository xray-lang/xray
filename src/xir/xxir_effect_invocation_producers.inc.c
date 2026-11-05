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
static XrXirStatus effect_invocation_return_supported(
    const XrXirTypes *types,XrXirType type,bool *output) {
    const XrXirTypeNode *signature=xr_xir_callable_signature(types,type);

    if (signature && (!xr_xir_callable_flags_valid(signature->flags) ||
        (!!signature->parameters!=!!signature->parameter_count))) return XR_XIR_BAD_TYPE;
    if(effect_physical_iterator(types,type)) {
        *output=!xr_xir_type_span(types,type);return XR_XIR_OK;
    }
    bool supported=signature && !signature->parameter_span &&
        (signature->flags&XR_XIR_CALLABLE_ROOT_MASK)==XR_XIR_CALLABLE_ROOT_NONE;
    for (uint32_t p=0;signature && p<signature->parameter_count;++p) {

        if (!xr_xir_callable_parameter_storage_valid(types,&signature->parameters[p]))
            return XR_XIR_BAD_TYPE;
        if (signature->parameters[p].mode!=XR_PARAM_READ) supported=false;
    }
    *output=supported;return XR_XIR_OK;
}
static XrXirStatus effect_invocation_producer_return_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,uint32_t body,bool *output) {
    if (!owner || !request || !output || body>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionHead head={0};XrXirStatus status=effect_body_request_head(request,body,&head);
    return status==XR_XIR_OK?effect_invocation_return_supported(request->domain.types,head.result,output):status;
}
/* Scalar, owned Cell and callable prefixes can propose identities. Their
 * actual environments still need complete finite support at publication;
 * an unsupported aggregate cannot acquire an invented empty environment. */
static XrXirStatus effect_invocation_producer_site_supported_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,uint32_t index,bool *output) {
    if (!owner || !request || !output || index>=owner->final->site_count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationSite *site=&owner->final->sites[index];
    if (site->target>=request->domain.function_count || site->function>=request->domain.function_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionSignature target_head={0};EffectBodyFunctionDimensions producer_head={0};
    XrXirStatus status=effect_body_request_signature(request,site->target,&target_head);
    if (status==XR_XIR_OK) status=effect_body_request_dimensions(request,site->function,&producer_head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionSignature *target=&target_head;
    const EffectBodyFunctionDimensions *producer=&producer_head;
    if (site->captures>target->parameter_count || site->instruction>=producer->instruction_count ||
        site->binding>owner->final->binding_count || site->captures>owner->final->binding_count-site->binding)
        return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    status=effect_body_request_operation(request,site->function,site->instruction,&storage,&op);
    if (status!=XR_XIR_OK) return status;
    if ((op->op!=XR_XIR_FUNCTION_REF && op->op!=XR_XIR_GENERATOR_NEW) ||
        op->immediate!=(int64_t)site->target || op->args[1]!=site->captures)
        return XR_XIR_BAD_STRUCTURE;
    bool supported=true;
    for (uint32_t p=0;p<site->captures;++p) {

        XrXirType type=target->parameters[p];
        if (xr_xir_type_span(request->domain.types,type)) supported=false;
        else if (xr_xir_type_is_cell(request->domain.types,type)) {
            if (xr_xir_cell_provenance_role(owner->final->cells,site->target,p)!=
                XR_XIR_CELL_PROOF_OWNED_CAPTURE) supported=false;
        }
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
    uint64_t origin_bits=(uint64_t)owner->final->site_count+basis->parameters+3;
    uint64_t stride=bits+origin_bits;
    if (owner->final->binding_count>(UINT32_MAX-UINT64_C(1))/stride) return XR_XIR_BUDGET;
    uint64_t atoms=(uint64_t)owner->final->binding_count*stride+1;
    if (atoms>UINT32_MAX) return XR_XIR_BUDGET;

    *output=(uint32_t)atoms;return XR_XIR_OK;
}

/* Capture atoms name typed columns of the one finite closure relation.
 * An enclosing producer can carry a nested binding's current columns; the
 * independent opcode replay must still prove every positive bit and cause. */
static XrXirStatus effect_invocation_producer_binding_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,uint32_t index,uint32_t binding,XrXirType *output) {
    if (!output || index>=owner->final->site_count || binding>=owner->final->binding_count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationSite *site=&owner->final->sites[index];
    if (binding<site->binding || binding-site->binding>=site->captures) return XR_XIR_BAD_STRUCTURE;
    if (!site || site->target>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionSignature target={0};
    XrXirStatus status=effect_body_request_signature(request,site->target,&target);
    if (status!=XR_XIR_OK) return status;
    uint32_t capture=binding-site->binding;
    if (capture>=target.parameter_count || !target.parameters) return XR_XIR_BAD_STRUCTURE;
    *output=target.parameters[capture];return XR_XIR_OK;
}
static XrXirStatus effect_invocation_producer_column_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,const EffectInvocationBasis *basis,
    uint32_t index,uint32_t atom,EffectInvocationColumn *output) {
    if (!owner || !request || !basis || !atom || !output || index>=owner->final->site_count)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t cell_bits=(uint64_t)basis->parameters+3;
    uint64_t origin_bits=(uint64_t)owner->final->site_count+basis->parameters+3;
    uint64_t cells=(uint64_t)owner->final->binding_count*cell_bits,code=(uint64_t)atom-1;
    bool origin=code>=cells;uint64_t bit=0,binding=0;
    if (origin) {code-=cells;binding=code/origin_bits;bit=code%origin_bits;}
    else {binding=code/cell_bits;bit=code%cell_bits;}

    if (binding>=owner->final->binding_count) return XR_XIR_BAD_STRUCTURE;
    XrXirType type=0;
    XrXirStatus status=effect_invocation_producer_binding_read(owner,request,index,(uint32_t)binding,&type);
    if (status!=XR_XIR_OK) return status;
    bool is_class=xr_xir_type_is_class(request->domain.types,type);
    if (origin ? !effect_physical_origin(request->domain.types,type) :
        ((!is_class && !xr_xir_type_is_cell(request->domain.types,type)) ||
            (is_class && bit<basis->parameters))) return XR_XIR_BAD_STRUCTURE;
    uint64_t column=basis->origin_words+binding*((uint64_t)basis->origin_words+basis->cell_words)+
        (origin?0:basis->origin_words)+bit/64;
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

    *word=column.word;*bit=column.bit;return XR_XIR_OK;
}

static const EffectInvocationProducer *effect_invocation_producer_find_atom(
    const EffectInvocationProducerSet *node,uint32_t value,uint32_t site,uint32_t atom) {
    for (uint32_t p=0;p<node->count;++p)
        if (node->facts[p].value==value && node->facts[p].site==site &&
            node->facts[p].atom==atom) return &node->facts[p];
    return NULL;
}
static inline const EffectInvocationProducer *effect_invocation_producer_find(
    const EffectInvocationProducerSet *node,uint32_t value,uint32_t site) {
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

/* Logical columns exclude storage padding. MAX-site records carry only a
 * current SSA bit; they have no producer identity, cause, rank or permission. */
/* Immutable input keys bound the facts that may enter a parameter. A
 * historical less precise key cannot acquire a site merely because a later
 * actual environment contains it; that actual must name another full key.
 * Producer deltas must revisit SSA transfers in already expanded equations. */
static XrXirStatus effect_invocation_producer_put(EffectInvocationFlow *flow,
    const EffectInvocationProducer *fact,bool *changed,bool verify) {
    const EffectInvocationRead *owner=flow->read;
    if (!changed || !fact || fact->value>flow->values || fact->site>=owner->final->site_count ||
        fact->distance==UINT64_MAX) return XR_XIR_BAD_STRUCTURE;
    /* Construction copies identity causes from admitted seeds. Complete site
     * support is independently replayed for every receiving record below. */
    bool supported=fact->atom || !verify;uint32_t atoms=0;XrXirStatus admitted=XR_XIR_OK;
    if (!supported) admitted=effect_invocation_producer_site_supported_read(owner,&flow->body,fact->site,&supported);
    if (admitted==XR_XIR_OK) admitted=effect_invocation_producer_atoms(owner,&flow->basis,&atoms);
    if (admitted!=XR_XIR_OK) return admitted;
    if (!supported || fact->atom>=atoms) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&owner->final->nodes[flow->node];
    const EffectInvocationProducerSet *current=effect_invocation_discovery_read(owner,flow->node);
    if(!current) return XR_XIR_BAD_STRUCTURE;
    if (fact->value<flow->values) {
        const uint64_t *row=NULL;
        if (flow->memory) row=effect_invocation_row(flow,fact->value);
        else if (fact->value<flow->function->parameter_count)
            row=node->input+(size_t)fact->value*flow->basis.row_words;
        if (!row) return XR_XIR_BAD_STRUCTURE;

        if (!fact->atom && !(row[fact->site/64]&(UINT64_C(1)<<(fact->site%64)))) return XR_XIR_OK;
        if (fact->atom) {
            uint32_t word=0,bit=0;
            admitted=effect_invocation_producer_column_flow(flow,fact->site,fact->atom,&word,&bit);
            if (admitted!=XR_XIR_OK) return admitted;
            if (!(row[word]&(UINT64_C(1)<<bit))) return XR_XIR_OK;
        }
    }
    for (uint32_t p=0;p<current->count;++p) {

        const EffectInvocationProducer *present=&current->facts[p];
        if (present->value!=fact->value || present->site!=fact->site || present->atom!=fact->atom) continue;
        if (verify) return present->distance<=fact->distance?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
        if (!effect_invocation_producer_before(fact,present)) return XR_XIR_OK;

        if (!flow->owner || owner!=&flow->owner->read) return XR_XIR_BAD_STRUCTURE;
        flow->owner->states[flow->node].producers.facts[p]=*fact;
        *changed=true;return effect_invocation_enqueue_precision(flow->owner,flow->node);
    }
    if (verify) return XR_XIR_BAD_STRUCTURE;
    if (!flow->owner || owner!=&flow->owner->read) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationProducerSet *receiving=&flow->owner->states[flow->node].producers;
    if (receiving->count==receiving->capacity) {
        uint64_t maximum=((uint64_t)flow->values+1)*owner->final->site_count;
        maximum=maximum>UINT32_MAX/atoms?UINT32_MAX:maximum*atoms;
        uint32_t capacity=receiving->capacity?receiving->capacity:8;
        if (capacity>maximum) capacity=(uint32_t)maximum;
        else if (receiving->capacity) capacity=capacity>maximum/2?(uint32_t)maximum:capacity*2;
        if (capacity<=receiving->count || (uint64_t)capacity>SIZE_MAX/sizeof(*fact)) return XR_XIR_BUDGET;
        XrXirStatus status=XR_XIR_OK;
        EffectInvocationProducer *records=xir_compile_alloc(owner->work,(size_t)capacity*sizeof(*records),&status);
        if (!records) return status;

        if (receiving->count) memcpy(records,receiving->facts,(size_t)receiving->count*sizeof(*records));
        xr_compile_resources_free(receiving->facts);receiving->facts=records;receiving->capacity=capacity;
    }

    receiving->facts[receiving->count++]=*fact;*changed=true;
    return effect_invocation_enqueue_precision(flow->owner,flow->node);
}

static XrXirStatus effect_invocation_producer_alias(EffectInvocationFlow *flow,
    uint32_t destination,uint32_t source,uint32_t instruction,bool *changed,bool verify) {
    if (destination>=flow->values || source>=flow->values) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationProducerSet *node=effect_invocation_discovery_read(flow->read,flow->node);
    if(!node) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<node->count;++p) {

        EffectInvocationProducer from=node->facts[p];
        if (from.value!=source || from.site==UINT32_MAX) continue;
        EffectInvocationProducer fact={destination,from.site,flow->node,source,UINT32_MAX,instruction,from.distance+1,from.atom};
        XrXirStatus status=effect_invocation_producer_put(flow,&fact,changed,verify);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* A global binding has exactly one declaration owner. This is a coordinate
 * lookup, not a choice among authority or closure alternatives. */
static XrXirStatus effect_invocation_capture_owner(const EffectInvocationRead *owner,
    uint32_t binding,uint32_t *output) {
    if (!output || binding>=owner->final->binding_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t found=UINT32_MAX;
    for (uint32_t s=0;s<owner->final->site_count;++s) {

        const EffectInvocationSite *site=&owner->final->sites[s];
        if (binding>=site->binding && binding-site->binding<site->captures) {
            if (found!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            found=s;
        }
    }
    if (found==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;

    *output=found;return XR_XIR_OK;
}

/* The source row comes from this opcode's full SSA replay. Nested origins,
 * including residual and symbolic bits, keep separate descending atoms. */
static XrXirStatus effect_invocation_producer_capture_atoms(EffectInvocationFlow *flow,
    EffectInvocationProducer *fact,const uint64_t *row,bool *changed,bool verify) {
    uint64_t cell_bits=(uint64_t)flow->basis.parameters+3;
    uint64_t origin_bits=(uint64_t)flow->read->final->site_count+flow->basis.parameters+3;
    uint64_t cells=(uint64_t)flow->read->final->binding_count*cell_bits;
    uint32_t binding_words=flow->basis.origin_words+flow->basis.cell_words;
    for (uint32_t w=flow->basis.origin_words;w<flow->basis.fn_words;++w) {

        uint64_t present=row[w];
        uint32_t relative=w-flow->basis.origin_words,binding=relative/binding_words;
        uint32_t column=relative%binding_words;bool origin=column<flow->basis.origin_words;
        uint32_t word=origin?column:column-flow->basis.origin_words;
        if (present) {
            XrXirStatus status=effect_invocation_capture_owner(flow->read,binding,&fact->site);
            if (status!=XR_XIR_OK) return status;
        }
        while (present) {

            uint32_t bit=(uint32_t)__builtin_ctzll(present);present&=present-1;
            uint64_t index=(uint64_t)word*64+bit;
            if (index>=(origin?origin_bits:cell_bits)) return XR_XIR_BAD_STRUCTURE;
            uint64_t code=1+(origin?cells+(uint64_t)binding*origin_bits:
                (uint64_t)binding*cell_bits)+index;
            if (code>UINT32_MAX) return XR_XIR_BUDGET;
            fact->atom=(uint32_t)code;uint32_t actual_word=0,actual_bit=0;
            XrXirStatus status=effect_invocation_producer_column_flow(flow,fact->site,
                fact->atom,&actual_word,&actual_bit);
            if (status!=XR_XIR_OK) return status;
            if (actual_word!=w || actual_bit!=bit) return XR_XIR_BAD_STRUCTURE;
            status=effect_invocation_producer_put(flow,fact,changed,verify);
            if (status!=XR_XIR_OK) return status;
        }
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
    const EffectInvocationSite *source=&flow->read->final->sites[site];
    EffectBodyFunctionSignature target_head={0};
    if (status==XR_XIR_OK) status=effect_body_request_signature(&flow->body,source->target,&target_head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionSignature *target=&target_head;
    const uint64_t *row=effect_invocation_row(flow,destination);
    if (!row) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<source->captures;++p) {

        if (effect_physical_origin(flow->body.domain.types,target->parameters[p]))
            return effect_invocation_producer_capture_atoms(flow,&fact,row,changed,verify);
    }
    uint64_t bits=(uint64_t)flow->basis.parameters+3;
    for (uint32_t p=0;status==XR_XIR_OK && p<source->captures;++p) {

        bool is_class=xr_xir_type_is_class(flow->body.domain.types,target->parameters[p]);
        if (!is_class && !xr_xir_type_is_cell(flow->body.domain.types,target->parameters[p])) continue;
        for (uint32_t b=is_class?flow->basis.parameters:0;status==XR_XIR_OK && b<bits;++b) {
            uint64_t code=1+((uint64_t)source->binding+p)*bits+b;
            if (code>UINT32_MAX) return XR_XIR_BUDGET;
            uint32_t word=0,bit=0;fact.atom=(uint32_t)code;
            status=effect_invocation_producer_column_flow(flow,site,fact.atom,&word,&bit);
            if (status!=XR_XIR_OK) break;
            if (row[word]&(UINT64_C(1)<<bit)) status=effect_invocation_producer_put(flow,&fact,changed,verify);
        }
    }
    return status;
}

static XrXirStatus effect_invocation_producer_callable_cell(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *read,bool *changed,bool verify) {
    uint32_t cell=UINT32_MAX;
    XrXirStatus status=effect_invocation_callable_cell(flow,read,&cell);
    if(status!=XR_XIR_OK || cell==UINT32_MAX) return status;
    EffectBodyRecordCursor cursor;
    status=effect_body_request_sequence_begin(&flow->body,&cursor);
    if(status!=XR_XIR_OK) return status;
    for(uint32_t i=0;i<flow->function->instruction_count;++i) {

        XrXirInstruction decoded;const XrXirInstruction *op=NULL;
        status=effect_body_request_sequence_operation(&flow->body,
            flow->read->final->nodes[flow->node].body,i,&cursor,&decoded,&op);
        uint32_t source=UINT32_MAX;
        if(status==XR_XIR_OK) status=effect_invocation_callable_cell_source(flow,cell,i,op,&source);
        if(status==XR_XIR_OK && source!=UINT32_MAX) status=effect_invocation_producer_alias(flow,
            flow->function->parameter_count+instruction,source,instruction,changed,verify);
        if(status!=XR_XIR_OK) return status;
    }
    return effect_body_request_sequence_finish(&flow->body,
        flow->read->final->nodes[flow->node].body,&cursor);
}

static XrXirStatus effect_invocation_producer_instruction(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *op,bool *changed,bool verify) {
    uint32_t destination=flow->function->parameter_count+instruction;
    switch (op->op) {
    case XR_XIR_FUNCTION_REF: case XR_XIR_GENERATOR_NEW:
        return effect_invocation_producer_seed(flow,instruction,changed,verify);
    case XR_XIR_CELL_READ:
        return effect_invocation_producer_callable_cell(flow,instruction,op,changed,verify);
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
        const EffectInvocationProducerSet *node=effect_invocation_discovery_read(flow->read,flow->node);
        if(!node) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p=0;p<node->count;++p) {

            EffectInvocationProducer from=node->facts[p];
            if (from.value!=op->args[0] || from.site==UINT32_MAX) continue;
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

/* A source is an actual local operand or one typed capture of the current
 * indirect receiver. Coordinates are not an identity or permission verdict. */
typedef struct EffectInvocationProducerSource {
    uint32_t value,producer,binding,parameter;
} EffectInvocationProducerSource;
static XrXirStatus effect_invocation_producer_source_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,const EffectInvocationEdge *edge,uint32_t parameter,
    EffectInvocationProducerSource *output) {
    if (!owner || !request || !edge || !output || edge->caller>=owner->final->count || edge->target>=owner->final->count)
        return XR_XIR_BAD_STRUCTURE;

    *output=(EffectInvocationProducerSource){UINT32_MAX,UINT32_MAX,UINT32_MAX,parameter};
    const EffectInvocationNode *caller=&owner->final->nodes[edge->caller],*target=&owner->final->nodes[edge->target];
    if (caller->body>=request->domain.function_count || target->body>=request->domain.function_count ||
        caller->root!=target->root) return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionHead function={0};EffectBodyFunctionDimensions callee={0};
    XrXirStatus status=effect_body_request_head(request,caller->body,&function);
    if (status==XR_XIR_OK) status=effect_body_request_dimensions(request,target->body,&callee);
    if (status!=XR_XIR_OK) return status;
    if (edge->instruction>=function.instruction_count || parameter>=callee.parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    status=effect_body_request_operation(request,caller->body,edge->instruction,&storage,&op);
    if (status!=XR_XIR_OK) return status;

    uint32_t captures=0;
    bool iterator=effect_invocation_iterator_operation(op->op);
    if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT || iterator) {
        if (edge->latent || edge->producer>=owner->final->site_count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationSite *site=&owner->final->sites[edge->producer];
        if (site->target!=target->body || site->binding>owner->final->binding_count ||
            site->captures>owner->final->binding_count-site->binding || site->captures>callee.parameter_count)
            return XR_XIR_BAD_STRUCTURE;
        captures=site->captures;
        if (iterator && captures!=callee.parameter_count) return XR_XIR_BAD_STRUCTURE;
        if (parameter<captures) {
            uint64_t value=iterator?op->args[0]:(uint64_t)op->immediate;
            if ((!iterator && op->immediate<0) || value>=(uint64_t)function.parameter_count+function.instruction_count)
                return XR_XIR_BAD_STRUCTURE;
            *output=(EffectInvocationProducerSource){(uint32_t)value,edge->producer,site->binding+parameter,parameter};
            return XR_XIR_OK;
        }
    } else if (op->op==XR_XIR_FUNCTION_REF) {
        if (!edge->latent || edge->producer>=owner->final->site_count ||
            owner->final->sites[edge->producer].function!=caller->body ||
            owner->final->sites[edge->producer].instruction!=edge->instruction ||
            owner->final->sites[edge->producer].target!=target->body || op->immediate<0 ||
            (uint64_t)op->immediate!=target->body) return XR_XIR_BAD_STRUCTURE;
        if (parameter>=op->args[1]) return XR_XIR_OK;
    } else if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_GO || op->op==XR_XIR_GENERATOR_NEW ||
        op->op==XR_XIR_CLEANUP_REGISTER) {
        if (edge->producer!=UINT32_MAX || edge->latent!=(op->op==XR_XIR_GO || op->op==XR_XIR_GENERATOR_NEW) ||
            op->immediate<0 || (uint64_t)op->immediate!=target->body) return XR_XIR_BAD_STRUCTURE;
    } else return XR_XIR_OK;
    uint32_t actual=parameter-captures;
    if (actual>=op->args[1] || !function.operands || op->args[0]>function.operand_count ||
        op->args[1]>function.operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
    actual=function.operands[op->args[0]+actual];
    if ((uint64_t)actual>=(uint64_t)function.parameter_count+function.instruction_count) return XR_XIR_BAD_VALUE;
    output->value=actual;return XR_XIR_OK;
}

/* Identity projection consumes the captured origin atom. Environment bits
 * keep their original global binding coordinate in the same complete row. */
static XrXirStatus effect_invocation_producer_source_atom(const EffectInvocationRead *owner,
    const EffectInvocationBasis *basis,const EffectInvocationProducerSource *source,
    uint32_t site,uint32_t atom,uint32_t *output) {
    if (!output || source->producer>=owner->final->site_count || source->binding>=owner->final->binding_count ||
        site>=owner->final->site_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t atoms=0;XrXirStatus status=effect_invocation_producer_atoms(owner,basis,&atoms);
    if (status!=XR_XIR_OK) return status;
    if (atom>=atoms) return XR_XIR_BAD_STRUCTURE;
    if (atom) {

        *output=atom;return XR_XIR_OK;
    }
    uint64_t cells=(uint64_t)owner->final->binding_count*((uint64_t)basis->parameters+3);
    uint64_t bits=(uint64_t)owner->final->site_count+basis->parameters+3;
    uint64_t code=1+cells+(uint64_t)source->binding*bits+site;
    if (code>UINT32_MAX) return XR_XIR_BUDGET;

    *output=(uint32_t)code;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_producer_capture_inputs(EffectInvocationFlow *flow,
    EffectInvocationFlow *projection,uint32_t edge_index,const EffectInvocationProducerSource *source,
    bool *changed,bool verify) {
    const EffectInvocationRead *owner=flow->read;
    const EffectInvocationProducerSet *caller=effect_invocation_discovery_read(owner,flow->node);
    if(!caller) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *target=&owner->final->nodes[projection->node];
    const uint64_t *row=target->input+(size_t)source->parameter*flow->basis.row_words;
    const EffectInvocationEdge edge=owner->final->edges[edge_index];
    for (uint32_t a=0;a<caller->count;++a) {

        const EffectInvocationProducer from=caller->facts[a];
        if (from.value!=source->value || !from.atom || from.site==UINT32_MAX) continue;
        EffectInvocationProducer fact={source->parameter,from.site,flow->node,source->value,edge_index,
            edge.instruction,from.distance+1,from.atom};
        XrXirStatus status=effect_invocation_producer_put(projection,&fact,changed,verify);
        if (status!=XR_XIR_OK) return status;
        if (from.site!=source->producer) continue;
        for (uint32_t site=0;site<owner->final->site_count;++site) {

            if (!(row[site/64]&(UINT64_C(1)<<(site%64)))) continue;
            bool supported=false;
            status=effect_invocation_producer_site_supported_read(owner,&flow->body,site,&supported);
            if (status!=XR_XIR_OK) return status;
            if (!supported) continue;
            uint32_t origin=0;
            status=effect_invocation_producer_source_atom(owner,&flow->basis,source,site,0,&origin);
            if (status!=XR_XIR_OK) return status;
            if (from.atom!=origin) continue;
            fact.site=site;fact.atom=0;
            status=effect_invocation_producer_put(projection,&fact,changed,verify);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
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

    EffectInvocationFlow projection={.owner=flow->owner,.read=owner,.node=edge.target,.function=function,
        .body=flow->body,.basis=flow->basis,.values=(uint32_t)values};
    for (uint32_t p=0;p<function->parameter_count;++p) {

        if (!effect_physical_origin(flow->body.domain.types,function->parameters[p])) continue;
        EffectInvocationProducerSource source={0};
        XrXirStatus status=effect_invocation_producer_source_read(owner,&flow->body,&edge,p,&source);
        if (status!=XR_XIR_OK) return status;
        if (source.value==UINT32_MAX) continue;
        if (source.producer!=UINT32_MAX) {
            status=effect_invocation_producer_capture_inputs(flow,&projection,edge_index,&source,changed,verify);
            if (status!=XR_XIR_OK) return status;
            continue;
        }
        uint32_t actual=source.value;
        const EffectInvocationProducerSet *caller=effect_invocation_discovery_read(owner,flow->node);
        if(!caller) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a=0;a<caller->count;++a) {

            EffectInvocationProducer from=caller->facts[a];
            if (from.value!=actual || from.site==UINT32_MAX) continue;
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
    const EffectInvocationProducerSet *records=effect_invocation_discovery_read(owner,edge.target);
    if(!records) return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionHead target_head={0};
    XrXirStatus read_status=effect_invocation_flow_head(flow,child->body,&target_head);
    if (read_status!=XR_XIR_OK) return read_status;
    const EffectBodyFunctionHead *target=&target_head;
    if (!effect_physical_origin(flow->body.domain.types,target->result)) return XR_XIR_OK;
    uint64_t returned=(uint64_t)target->parameter_count+target->instruction_count;
    EffectBodyRecordCursor cursor;
    bool owned=effect_body_request_owned_matches(owner->work,&flow->body,owner->final->material);
    if (!owned) read_status=effect_body_request_sequence_begin(&flow->body,&cursor);
    if (read_status!=XR_XIR_OK) return read_status;
    uint32_t count=owned?flow->physical->count:flow->function->instruction_count;
    for (uint32_t r=0;r<count;++r) {
        uint32_t i=r;
        if (owned) {

            const EffectPhysicalRecord record=flow->physical->records[r];
            if (!(record.mask&(EFFECT_PHYSICAL_CALLABLE|EFFECT_PHYSICAL_ITERATOR)) ||
                record.value<flow->function->parameter_count) continue;
            if (record.value>=flow->values) return XR_XIR_BAD_STRUCTURE;
            i=record.value-flow->function->parameter_count;
        }

        XrXirInstruction body_op_2;const XrXirInstruction *op=NULL;
        XrXirStatus body_read_2=owned?effect_invocation_flow_instruction(flow,i,&body_op_2,&op):
            effect_body_request_sequence_operation(&flow->body,
                owner->final->nodes[flow->node].body,i,&cursor,&body_op_2,&op);
        if (body_read_2!=XR_XIR_OK) return body_read_2;
        if (!effect_physical_origin(flow->body.domain.types,op->type)) continue;
        bool source=(op->op==XR_XIR_INVOKE_RESULT && op->immediate==(int64_t)edge.instruction) ||
            (i==edge.instruction && (op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_DEFAULT ||
            op->op==XR_XIR_CALL_INDIRECT));
        if (!source) continue;
        for (uint32_t p=0;p<records->count;++p) {

            EffectInvocationProducer from=records->facts[p];
            if (from.value!=returned || from.site==UINT32_MAX) continue;
            EffectInvocationProducer fact={flow->function->parameter_count+i,from.site,
                edge.target,(uint32_t)returned,edge_index,i,from.distance+1,from.atom};
            XrXirStatus status=effect_invocation_producer_put(flow,&fact,changed,verify);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return owned?XR_XIR_OK:effect_body_request_sequence_finish(&flow->body,owner->final->nodes[flow->node].body,&cursor);
}

/* Every producer-bearing transfer and RETURN is in the canonical physical
 * inventory. Scalars outside that inventory contribute no producer clause;
 * each selected operation still supplies its complete current operands. */
static XrXirStatus effect_invocation_producer_owned_instructions(EffectInvocationFlow *flow,
    bool *changed,bool verify) {
    for (uint32_t r=0;r<flow->physical->count;++r) {

        const EffectPhysicalRecord record=flow->physical->records[r];
        if (!(record.mask&(EFFECT_PHYSICAL_TRANSFER|EFFECT_PHYSICAL_RETURN))) continue;
        if (record.value<flow->function->parameter_count || record.value>=flow->values) return XR_XIR_BAD_STRUCTURE;
        uint32_t i=record.value-flow->function->parameter_count;
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        XrXirStatus status=effect_invocation_flow_instruction(flow,i,&storage,&op);
        if (status==XR_XIR_OK) status=effect_invocation_producer_instruction(flow,i,op,changed,verify);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_producer_step(EffectInvocationFlow *flow,
    bool *changed,bool verify) {
    if (!flow || !changed || !flow->read->producer_enabled) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationRead *owner=flow->read;EffectBodyRecordCursor cursor;
    if(!flow->row_count) return XR_XIR_OK;
    XrXirStatus read_status=XR_XIR_OK;
    if (effect_body_request_owned_matches(owner->work,&flow->body,owner->final->material))
        read_status=effect_invocation_producer_owned_instructions(flow,changed,verify);
    else {
        read_status=effect_body_request_sequence_begin(&flow->body,&cursor);
        if (read_status!=XR_XIR_OK) return read_status;
        for (uint32_t i=0;i<flow->function->instruction_count;++i) {

            XrXirInstruction storage;const XrXirInstruction *op=NULL;
            XrXirStatus status=effect_body_request_sequence_operation(&flow->body,
                owner->final->nodes[flow->node].body,i,&cursor,&storage,&op);
            if (status==XR_XIR_OK) status=effect_invocation_producer_instruction(flow,i,op,changed,verify);
            if (status!=XR_XIR_OK) return status;
        }
        read_status=effect_body_request_sequence_finish(&flow->body,owner->final->nodes[flow->node].body,&cursor);
    }
    if (read_status!=XR_XIR_OK) return read_status;
    for (uint32_t e=owner->final->nodes[flow->node].edge_head;e!=UINT32_MAX;e=owner->final->edges[e].next) {

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
static XrXirStatus effect_invocation_producer_callable_cell_cause(EffectInvocationFlow *flow,
    const EffectInvocationProducer *fact,const XrXirInstruction *read) {
    uint32_t cell=UINT32_MAX;
    XrXirStatus status=effect_invocation_callable_cell(flow,read,&cell);
    if(status!=XR_XIR_OK) return status;
    if(cell==UINT32_MAX || fact->value!=flow->function->parameter_count+fact->instruction)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyRecordCursor cursor;
    status=effect_body_request_sequence_begin(&flow->body,&cursor);
    if(status!=XR_XIR_OK) return status;
    bool found=false;
    for(uint32_t i=0;i<flow->function->instruction_count;++i) {

        XrXirInstruction decoded;const XrXirInstruction *op=NULL;
        status=effect_body_request_sequence_operation(&flow->body,
            flow->read->final->nodes[flow->node].body,i,&cursor,&decoded,&op);
        uint32_t source=UINT32_MAX;
        if(status==XR_XIR_OK) status=effect_invocation_callable_cell_source(flow,cell,i,op,&source);
        if(status!=XR_XIR_OK) return status;
        if(source!=UINT32_MAX && source==fact->next_value) found=true;
    }
    status=effect_body_request_sequence_finish(&flow->body,
        flow->read->final->nodes[flow->node].body,&cursor);
    return status!=XR_XIR_OK?status:found?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

typedef struct EffectInvocationProducerKey {uint32_t site,atom;} EffectInvocationProducerKey;
static XrXirStatus effect_invocation_producer_cause(EffectInvocationFlow *flow,
    const EffectInvocationProducer *fact,EffectInvocationProducerKey *key) {
    const EffectInvocationRead *owner=flow->read;
    const EffectBodyFunctionHead *head=flow->function;const EffectBodyRequest *body=&flow->body;
    uint32_t selected=flow->node,values=flow->values;
    if (!head || selected>=owner->final->count ||
        (uint64_t)head->parameter_count+head->instruction_count!=values) return XR_XIR_BAD_STRUCTURE;
    if (!key) return XR_XIR_BAD_STRUCTURE;
    *key=(EffectInvocationProducerKey){fact->site,fact->atom};
    if (fact->next_node>=owner->final->count) return XR_XIR_BAD_STRUCTURE;

    if (fact->edge!=UINT32_MAX) {
        if (fact->edge>=owner->final->edge_count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationEdge *edge=&owner->final->edges[fact->edge];
        if (fact->value<head->parameter_count) {
            if (edge->target!=selected || edge->caller!=fact->next_node ||
                edge->instruction!=fact->instruction) return XR_XIR_BAD_STRUCTURE;
            EffectInvocationProducerSource source={0};
            XrXirStatus status=effect_invocation_producer_source_read(owner,&flow->body,edge,fact->value,&source);
            if (status!=XR_XIR_OK) return status;
            if (source.value==UINT32_MAX || source.value!=fact->next_value) return XR_XIR_BAD_STRUCTURE;
            if (source.producer!=UINT32_MAX) {
                key->site=fact->atom?fact->site:source.producer;
                status=effect_invocation_producer_source_atom(owner,&flow->basis,&source,fact->site,fact->atom,&key->atom);
            }
            return status;
        }
        if (edge->caller!=selected || edge->target!=fact->next_node || edge->latent ||
            fact->value>=values || fact->instruction>=head->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        EffectBodyFunctionDimensions target_head={0};
        XrXirStatus head_status=effect_body_request_dimensions(&flow->body,owner->final->nodes[fact->next_node].body,&target_head);
        if (head_status!=XR_XIR_OK) return head_status;
        const EffectBodyFunctionDimensions *target=&target_head;
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
    case XR_XIR_CELL_READ:
        return effect_invocation_producer_callable_cell_cause(flow,fact,op);
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_SCALAR_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_NEW:
    case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_READ:
        return fact->value==destination && fact->next_value==op->args[0]?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_WRITE:
        return fact->value==op->args[0] && fact->next_value==op->args[1]?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    case XR_XIR_PHI:
        if (fact->value!=destination) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a=1;a<op->args[1];a+=2) {

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
    bool supported=fact->atom!=0;uint32_t atoms=0;
    XrXirStatus admitted=supported?XR_XIR_OK:
        effect_invocation_producer_site_supported_read(owner,&flow->body,fact->site,&supported);
    if (admitted==XR_XIR_OK) admitted=effect_invocation_producer_atoms(owner,&flow->basis,&atoms);
    if (admitted!=XR_XIR_OK) return admitted;
    if (!supported || fact->atom>=atoms) return XR_XIR_BAD_STRUCTURE;

    const uint64_t *row=fact->value<flow->values ? effect_invocation_row(flow,fact->value) : NULL;
    if (fact->value<flow->values && (!row || (!fact->atom && !(row[fact->site/64]&
        (UINT64_C(1)<<(fact->site%64)))))) return XR_XIR_BAD_STRUCTURE;
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
        uint32_t seed=UINT32_MAX;
        if (fact->instruction>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=effect_invocation_site(flow,fact->instruction,&seed);
        if (status!=XR_XIR_OK) return status;
        return (fact->atom || seed==fact->site) &&
            fact->value==flow->function->parameter_count+fact->instruction && certified &&
            fact->next_node==UINT32_MAX && fact->next_value==UINT32_MAX && fact->edge==UINT32_MAX?
            XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    EffectInvocationProducerKey key={0};
    XrXirStatus status=effect_invocation_producer_cause(flow,fact,&key);
    if (status!=XR_XIR_OK) return status;
    const EffectInvocationProducerSet *next=effect_invocation_discovery_read(owner,fact->next_node);
    if(!next) return XR_XIR_BAD_STRUCTURE;
    for(uint32_t p=0;p<next->count;++p) {

        const EffectInvocationProducer *cause=&next->facts[p];
        if(cause->value==fact->next_value && cause->site==key.site && cause->atom==key.atom)
            return cause->distance+1==fact->distance?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_BAD_STRUCTURE;
}

/* No ROOT result is changed during this replay. Inflation, an unrooted cycle,
 * missing transfers and forged actual projections fail before owner transfer.
 * The descending chain is private and never enters the public function forest. */
static XrXirStatus effect_invocation_discovery_verify(const EffectInvocationWorkspace *discovery,
    const EffectBodyRequest *request) {
    if(!discovery || discovery->read.discovery!=discovery || !discovery->states) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationRead *owner=&discovery->read;
    if (!owner || !owner->work || !request || !request->owner || !request->domain.types) return XR_XIR_BAD_STRUCTURE;
    uint64_t maximum=0;bool enabled=owner->final->site_count!=0;
    if(enabled!=owner->producer_enabled) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t n=0;n<owner->final->count;++n) {

        const EffectInvocationNode *node=&owner->final->nodes[n];
        const EffectInvocationProducerSet *records=effect_invocation_discovery_read(owner,n);
        if(!records) return XR_XIR_BAD_STRUCTURE;
        if (node->body>=request->domain.function_count || records->count>records->capacity ||
            (!!records->facts!=!!records->capacity) || (!enabled && records->count))
            return XR_XIR_BAD_STRUCTURE;
        EffectBodyFunctionDimensions body_head={0};
        XrXirStatus head_status=effect_body_request_dimensions(request,node->body,&body_head);
        if (head_status!=XR_XIR_OK) return head_status;

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
        if (records->capacity>keys || maximum>UINT64_MAX-records->count)
            return XR_XIR_BAD_STRUCTURE;
        /* Validate coordinates before origin projection can consume a record.
         * A real Cell environment still enters the complete charged replay. */
        for (uint32_t p=0;p<records->count;++p) {

            const EffectInvocationProducer *fact=&records->facts[p];
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
        maximum+=records->count;
    }
    if(!enabled) return XR_XIR_OK;
    for (uint32_t n=0;n<owner->final->count;++n) {
        EffectInvocationFlow flow={.read=owner,.node=n,.instruction=UINT32_MAX};
        XrXirStatus status=effect_invocation_discovery_origins_read(&flow,request);
        const EffectInvocationProducerSet *records=effect_invocation_discovery_read(owner,n);
        if(!records) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p=0;status==XR_XIR_OK && p<records->count;++p) {
            const EffectInvocationProducer *fact=&records->facts[p];
            for (uint32_t a=0;a<p;++a) {

                if (records->facts[a].value==fact->value && records->facts[a].site==fact->site &&
                    records->facts[a].atom==fact->atom) {
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
    return effect_invocation_discovery_verify(owner,&request);
}
