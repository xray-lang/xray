/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_nodes.inc.c - Owned keys, fixed producer basis and one shared equation queue
 */
/* The producer basis is fixed before any equation is expanded. Capture
 * environments are flat rows for these real sites; a repeated nested site
 * cannot manufacture another basis atom or a recursively copied path. */
static XrXirStatus effect_invocation_sites(EffectInvocationOwner *owner) {
    if (!owner || !owner->module || !owner->work || owner->sites || owner->site_count ||
        owner->binding_count) return XR_XIR_BAD_STRUCTURE;
    uint64_t count=0,bindings=0;
    for (uint32_t f=0;f<owner->module->function_count;++f) {
        const XrXirFunction *function=&owner->module->functions[f];
        for (uint32_t i=0;i<function->instruction_count;++i) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            const XrXirInstruction *op=&function->instructions[i];
            if (op->op!=XR_XIR_FUNCTION_REF) continue;
            if (op->immediate<0 || (uint64_t)op->immediate>=owner->module->function_count ||
                op->args[1]>owner->module->functions[op->immediate].parameter_count)
                return XR_XIR_BAD_STRUCTURE;
            ++count;bindings+=op->args[1];
            if (count>UINT32_MAX || bindings>UINT32_MAX) return XR_XIR_BUDGET;
        }
    }
    if (count>SIZE_MAX/sizeof(EffectInvocationSite)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    EffectInvocationSite *sites=count ? xir_compile_alloc(owner->work,
        (size_t)count*sizeof(*sites),&status) : NULL;
    if (count && !sites) return status;
    uint32_t at=0,binding=0;
    for (uint32_t f=0;f<owner->module->function_count && status==XR_XIR_OK;++f) {
        const XrXirFunction *function=&owner->module->functions[f];
        for (uint32_t i=0;i<function->instruction_count;++i) {
            if (!xir_compile_work(owner->work,1)) { status=XR_XIR_BUDGET;break; }
            const XrXirInstruction *op=&function->instructions[i];
            if (op->op!=XR_XIR_FUNCTION_REF) continue;
            if (!xir_compile_work(owner->work,sizeof(*sites))) { status=XR_XIR_BUDGET;break; }
            if (at>=count || binding>bindings || op->args[1]>bindings-binding) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
            sites[at++]=(EffectInvocationSite){f,i,(uint32_t)op->immediate,op->args[1],binding};
            binding+=op->args[1];
        }
    }
    if (status==XR_XIR_OK && (at!=count || binding!=bindings)) status=XR_XIR_BAD_STRUCTURE;
    if (status!=XR_XIR_OK) { xr_compile_resources_free(sites);return status; }
    owner->sites=sites;owner->site_count=at;owner->binding_count=binding;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_basis(const EffectInvocationOwner *owner,
    uint32_t root,EffectInvocationBasis *output) {
    if (root>=owner->module->function_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t parameters=owner->module->functions[root].parameter_count;
    uint64_t fn_bits=(uint64_t)owner->site_count+parameters+3;
    uint64_t cell_bits=(uint64_t)parameters+3;
    uint64_t origin_words=(fn_bits+63)/64,cell_words=(cell_bits+63)/64;
    uint64_t fn_words=origin_words+(uint64_t)owner->binding_count*(origin_words+cell_words);
    if (fn_bits>UINT32_MAX || cell_bits>UINT32_MAX ||
        fn_words>UINT32_MAX || cell_words>UINT32_MAX ||
        fn_words+cell_words>UINT32_MAX) return XR_XIR_BUDGET;
    if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
    *output=(EffectInvocationBasis){parameters,(uint32_t)origin_words,(uint32_t)fn_words,
        (uint32_t)cell_words,(uint32_t)(fn_words+cell_words)};
    return XR_XIR_OK;
}

static void effect_invocation_free(EffectInvocationOwner *owner) {
    if (!owner) return;
    for (uint32_t n=0;n<owner->count;++n) {
        xr_compile_resources_free(owner->nodes[n].causes);
        xr_compile_resources_free(owner->nodes[n].producers);
    }
    for (uint32_t d=0;d<owner->deferred_count;++d)
        xr_compile_resources_free(owner->deferred_calls[d].arguments);
    xr_compile_resources_free(owner->deferred_calls);
    xr_compile_resources_free(owner->nodes);
    xr_compile_resources_free(owner->edges);
    xr_compile_resources_free(owner->consumers);
    xr_compile_resources_free(owner->sites);
    xr_compile_resources_free(owner->roots);xr_compile_resources_free(owner->root_spaces);
    xr_compile_resources_free(owner->local_roots);
    xr_compile_resources_free(owner->local_witnesses);
    xr_compile_resources_free(owner);
}

/* The formula graph's queue is shared by every invocation equation. All
 * live entries retain source order during growth; no second queue solves ROOT. */
static XrXirStatus effect_invocation_capacity(EffectInvocationOwner *owner) {
    if (owner->count<owner->capacity) return XR_XIR_OK;
    if (owner->count>=owner->maximum) return XR_XIR_BUDGET;
    uint32_t capacity=owner->capacity ? owner->capacity : 8;
    if (capacity>owner->maximum) capacity=owner->maximum;
    else if (owner->capacity) capacity=capacity>owner->maximum/2 ?
        owner->maximum : capacity*2;
    uint64_t node_bytes=(uint64_t)capacity*sizeof(*owner->nodes);
    uint64_t index_bytes=(uint64_t)capacity*sizeof(*owner->consumers);
    if (capacity<=owner->count || node_bytes>SIZE_MAX ||
        index_bytes>SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    EffectInvocationNode *nodes=xir_compile_calloc(owner->work,capacity,sizeof(*nodes),&status);
    uint32_t *consumers=status==XR_XIR_OK ?
        xir_compile_alloc(owner->work,(size_t)capacity*sizeof(*consumers),&status) : NULL;
    uint32_t *queue=status==XR_XIR_OK ?
        xir_compile_alloc(owner->work,(size_t)capacity*sizeof(*queue),&status) : NULL;
    if (!nodes || !consumers || !queue) {
        xr_compile_resources_free(nodes);xr_compile_resources_free(consumers);
        xr_compile_resources_free(queue);return status;
    }
    uint64_t work=(uint64_t)owner->count*(sizeof(*nodes)+sizeof(*consumers))+
        (uint64_t)owner->pending*sizeof(*queue)+capacity;
    if (!xir_compile_work(owner->work,work)) {
        xr_compile_resources_free(nodes);xr_compile_resources_free(consumers);
        xr_compile_resources_free(queue);return XR_XIR_BUDGET;
    }
    if (owner->count) {
        memcpy(nodes,owner->nodes,(size_t)owner->count*sizeof(*nodes));
        memcpy(consumers,owner->consumers,(size_t)owner->count*sizeof(*consumers));
    }
    for (uint32_t n=owner->count;n<capacity;++n) consumers[n]=UINT32_MAX;
    for (uint32_t q=0;q<owner->pending;++q) {
        uint64_t at=(uint64_t)owner->front+q;
        if (at>=owner->capacity) at-=owner->capacity;
        queue[q]=owner->graph->queue[at];
    }
    xr_compile_resources_free(owner->nodes);xr_compile_resources_free(owner->consumers);
    xr_compile_resources_free(owner->graph->queue);
    owner->nodes=nodes;owner->consumers=consumers;owner->graph->queue=queue;
    owner->capacity=capacity;owner->front=0;
    owner->back=owner->pending==capacity ? 0 : owner->pending;
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_enqueue(EffectInvocationOwner *owner,uint32_t node) {
    if (node>=owner->count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
    if (owner->nodes[node].queued) return XR_XIR_OK;
    if (owner->pending>=owner->capacity) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
    owner->graph->queue[owner->back]=node;
    owner->back=owner->back+1==owner->capacity ? 0 : owner->back+1;
    ++owner->pending;owner->nodes[node].queued=true;return XR_XIR_OK;
}

/* Neither a producer pointer nor a numeric target alone is a key. Exact
 * physical origin rows are copied into this owner before the node is linked. */
static XrXirStatus effect_invocation_intern(EffectInvocationOwner *owner,
    uint32_t root,uint32_t body,const uint64_t *input,uint32_t *output) {
    if (body>=owner->module->function_count || !output) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationBasis basis={0};XrXirStatus status=effect_invocation_basis(owner,root,&basis);
    if (status!=XR_XIR_OK) return status;
    uint32_t parameters=owner->module->functions[body].parameter_count;
    uint64_t words=(uint64_t)parameters*basis.row_words;
    if (words>SIZE_MAX/sizeof(*input) || (words && !input)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t n=0;n<owner->count;++n) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&owner->nodes[n];
        if (node->root!=root || node->body!=body || node->parameter_count!=parameters) continue;
        bool same=true;
        for (size_t w=0;w<(size_t)words;++w) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            if (node->input[w]!=input[w]) same=false;
        }
        if (same) { *output=n;return XR_XIR_OK; }
    }
    status=effect_invocation_capacity(owner);
    if (status!=XR_XIR_OK) return status;
    uint32_t dependency_words=(uint32_t)(((uint64_t)basis.parameters+63)/64);
    uint32_t context_words=(uint32_t)(((uint64_t)owner->module->functions[body].instruction_count+63)/64);
    uint64_t output_words=(uint64_t)dependency_words*2+context_words;
    uint64_t cause_count=(uint64_t)basis.parameters*2+3;
    if (output_words>SIZE_MAX/(2*sizeof(uint64_t)) || cause_count>UINT32_MAX ||
        cause_count>SIZE_MAX/(2*sizeof(EffectInvocationCause))) return XR_XIR_BUDGET;
    size_t input_bytes=(size_t)words*sizeof(uint64_t);
    size_t output_bytes=(size_t)output_words*2*sizeof(uint64_t);
    size_t cause_bytes=(size_t)cause_count*2*sizeof(EffectInvocationCause);
    if (input_bytes>SIZE_MAX-cause_bytes || output_bytes>SIZE_MAX-cause_bytes-input_bytes)
        return XR_XIR_BUDGET;
    /* These rows share exactly one equation lifetime. The cause pointer is
     * the allocation root; every input and output pointer is an interior view. */
    EffectInvocationCause *causes=xir_compile_calloc(owner->work,1,
        cause_bytes+input_bytes+output_bytes,&status);
    if (!causes) return status;
    uint8_t *memory=(uint8_t *)causes;
    uint64_t *owned=words ? (uint64_t *)(memory+cause_bytes) : NULL;
    uint64_t *outputs=output_words ? (uint64_t *)(memory+cause_bytes+input_bytes) : NULL;
    uint64_t *cells=outputs,*callables=outputs ? outputs+dependency_words : NULL;
    uint64_t *contexts=outputs ? outputs+(size_t)dependency_words*2 : NULL;
    uint64_t *local_cells=outputs ? outputs+(size_t)output_words : NULL;
    uint64_t *local_parameters=local_cells ? local_cells+dependency_words : NULL;
    uint64_t *local_contexts=local_cells ? local_cells+(size_t)dependency_words*2 : NULL;
    if (!xir_compile_work(owner->work,cause_count)) {
        xr_compile_resources_free(causes);return XR_XIR_BUDGET;
    }
    for (uint32_t a=0;a<(uint32_t)cause_count;++a) {
        causes[a].next=causes[a].edge=causes[a].distance=UINT32_MAX;
    }
    if (!xir_compile_work(owner->work,words*sizeof(*owned)+sizeof(EffectInvocationNode))) {
        xr_compile_resources_free(causes);return XR_XIR_BUDGET;
    }
    if (words) memcpy(owned,input,(size_t)words*sizeof(*owned));
    uint32_t at=owner->count;
    owner->nodes[at]=(EffectInvocationNode){.root=root,.body=body,
        .parameter_count=parameters,.input=owned,.outputs=outputs,
        .causes=causes,.local_causes=causes+(size_t)cause_count,.cause_count=(uint32_t)cause_count,
        .cells=cells,.parameters=callables,.contexts=contexts,
        .local_cells=local_cells,.local_parameters=local_parameters,.local_contexts=local_contexts,
        .edge_head=UINT32_MAX};
    ++owner->count;
    status=effect_invocation_enqueue(owner,at);
    if (status!=XR_XIR_OK) return status;
    *output=at;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_edge(EffectInvocationOwner *owner,
    uint32_t caller,uint32_t target,uint32_t instruction,uint32_t producer,bool latent) {
    if (caller>=owner->count || target>=owner->count ||
        owner->nodes[caller].root!=owner->nodes[target].root ||
        instruction>=owner->module->functions[owner->nodes[caller].body].instruction_count ||
        (producer!=UINT32_MAX && (producer>=owner->site_count ||
            owner->sites[producer].target!=owner->nodes[target].body)))
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t e=owner->nodes[caller].edge_head;e!=UINT32_MAX;e=owner->edges[e].next) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (owner->edges[e].target==target && owner->edges[e].instruction==instruction &&
            owner->edges[e].producer==producer && owner->edges[e].latent==latent)
            return XR_XIR_OK;
    }
    if (owner->edge_count==owner->edge_capacity) {
        uint32_t maximum=owner->maximum;
        if ((uint64_t)maximum>SIZE_MAX/sizeof(*owner->edges)) return XR_XIR_BUDGET;
        uint32_t capacity=owner->edge_capacity ? owner->edge_capacity : 8;
        if (capacity>maximum) capacity=maximum;
        else if (owner->edge_capacity) capacity=capacity>maximum/2 ? maximum : capacity*2;
        if (capacity<=owner->edge_count) return XR_XIR_BUDGET;
        XrXirStatus status=XR_XIR_OK;
        EffectInvocationEdge *edges=xir_compile_alloc(owner->work,
            (size_t)capacity*sizeof(*edges),&status);
        if (!edges) return status;
        if (!xir_compile_work(owner->work,(uint64_t)owner->edge_count*sizeof(*edges))) {
            xr_compile_resources_free(edges);return XR_XIR_BUDGET;
        }
        if (owner->edge_count) memcpy(edges,owner->edges,(size_t)owner->edge_count*sizeof(*edges));
        xr_compile_resources_free(owner->edges);owner->edges=edges;owner->edge_capacity=capacity;
    }
    if (!xir_compile_work(owner->work,sizeof(EffectInvocationEdge)+2)) return XR_XIR_BUDGET;
    uint32_t at=owner->edge_count;
    owner->edges[at]=(EffectInvocationEdge){caller,target,instruction,producer,
        owner->nodes[caller].edge_head,owner->consumers[target],latent};
    owner->nodes[caller].edge_head=at;owner->consumers[target]=at;
    ++owner->edge_count;
    /* A completed child's producer can predate a newly linked real edge. */
    return owner->producer_enabled ? effect_invocation_enqueue(owner,caller) : XR_XIR_OK;
}

static XrXirStatus effect_invocation_join(EffectInvocationOwner *owner,
    uint32_t caller,uint32_t target,uint32_t instruction,bool *changed) {
    if (caller>=owner->count || target>=owner->count || !changed ||
        owner->nodes[caller].root!=owner->nodes[target].root) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationNode *to=&owner->nodes[caller];
    const EffectInvocationNode *from=&owner->nodes[target];
    EffectInvocationBasis basis={0};XrXirStatus status=effect_invocation_basis(owner,to->root,&basis);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
    uint32_t mask=to->intrinsic_mask|from->intrinsic_mask;
    bool deferred=to->deferred||from->deferred;
    if (mask!=to->intrinsic_mask || deferred!=to->deferred) *changed=true;
    to->intrinsic_mask=mask;to->deferred=deferred;
    if (from->deferred) {
        uint32_t instructions=owner->module->functions[to->body].instruction_count;
        if (instruction>=instructions || !to->contexts) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        uint64_t bit=UINT64_C(1)<<(instruction%64);
        if (!(to->contexts[instruction/64]&bit)) *changed=true;
        to->contexts[instruction/64]|=bit;
    }
    uint32_t words=(uint32_t)(((uint64_t)basis.parameters+63)/64);
    for (uint32_t w=0;w<words;++w) {
        if (!xir_compile_work(owner->work,6)) return XR_XIR_BUDGET;
        uint64_t cells=to->cells[w]|from->cells[w];
        uint64_t parameters=to->parameters[w]|from->parameters[w];
        if (cells!=to->cells[w] || parameters!=to->parameters[w]) *changed=true;
        to->cells[w]=cells;to->parameters[w]=parameters;
    }
    return XR_XIR_OK;
}
