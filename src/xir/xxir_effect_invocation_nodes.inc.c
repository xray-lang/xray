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
static XrXirStatus effect_invocation_sites(EffectInvocationWorkspace *owner) {
    if (!owner || !effect_body_material_shape(owner->final.material) || !owner->work || owner->final.sites || owner->final.site_count ||
        owner->final.binding_count || !owner->final.physical ||
        owner->final.physical->resources!=owner->work->resources ||
        owner->final.physical->count!=owner->body.domain.function_count ||
        (owner->final.physical->count && !owner->final.physical->functions)) return XR_XIR_BAD_STRUCTURE;
    uint64_t count=0,bindings=0;
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t f=0;f<owner->body.domain.function_count;++f) {
        EffectBodyFunctionDimensions head={0};status=effect_invocation_dimensions(owner,f,&head);
        if (status!=XR_XIR_OK) return status;
        const EffectBodyFunctionDimensions *function=&head;
        const EffectPhysicalFunction *physical=&owner->final.physical->functions[f];
        for (uint32_t r=0;r<physical->count;++r) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            if (!(physical->records[r].mask&EFFECT_PHYSICAL_REFERENCE)) continue;
            uint32_t value=physical->records[r].value;
            if (value<function->parameter_count || value>=physical->values) return XR_XIR_BAD_STRUCTURE;
            uint32_t i=value-function->parameter_count;
            XrXirInstruction storage;const XrXirInstruction *op=NULL;
            status=effect_invocation_operation(owner,f,i,&storage,&op);
            if (status!=XR_XIR_OK) return status;
            if (op->op!=XR_XIR_FUNCTION_REF || op->immediate<0 ||
                (uint64_t)op->immediate>=owner->body.domain.function_count) return XR_XIR_BAD_STRUCTURE;
            EffectBodyFunctionDimensions target={0};
            status=effect_invocation_dimensions(owner,(uint32_t)op->immediate,&target);
            if (status!=XR_XIR_OK) return status;
            if (op->args[1]>target.parameter_count) return XR_XIR_BAD_STRUCTURE;
            ++count;bindings+=op->args[1];
            if (count>UINT32_MAX || bindings>UINT32_MAX) return XR_XIR_BUDGET;
        }
    }
    if (count>SIZE_MAX/sizeof(EffectInvocationSite)) return XR_XIR_BUDGET;
    EffectInvocationSite *sites=count ? xir_compile_alloc(owner->work,
        (size_t)count*sizeof(*sites),&status) : NULL;
    if (count && !sites) return status;
    uint32_t at=0,binding=0;
    for (uint32_t f=0;f<owner->body.domain.function_count && status==XR_XIR_OK;++f) {
        EffectBodyFunctionDimensions head={0};status=effect_invocation_dimensions(owner,f,&head);
        if (status!=XR_XIR_OK) break;
        const EffectBodyFunctionDimensions *function=&head;
        const EffectPhysicalFunction *physical=&owner->final.physical->functions[f];
        for (uint32_t r=0;r<physical->count;++r) {
            if (!xir_compile_work(owner->work,1)) { status=XR_XIR_BUDGET;break; }
            if (!(physical->records[r].mask&EFFECT_PHYSICAL_REFERENCE)) continue;
            uint32_t value=physical->records[r].value;
            if (value<function->parameter_count || value>=physical->values) { status=XR_XIR_BAD_STRUCTURE;break; }
            uint32_t i=value-function->parameter_count;
            XrXirInstruction storage;const XrXirInstruction *op=NULL;
            status=effect_invocation_operation(owner,f,i,&storage,&op);
            if (status!=XR_XIR_OK) break;
            if (op->op!=XR_XIR_FUNCTION_REF) { status=XR_XIR_BAD_STRUCTURE;break; }
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
    owner->final.sites=sites;owner->final.site_count=at;owner->final.binding_count=binding;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_basis_parameters_read(const XrXirCompileContext *work,
    const EffectInvocationFinal *final,
    uint32_t parameters,EffectInvocationBasis *output) {
    if (!xir_compile_context_valid(work) || !final || !output) return XR_XIR_BAD_STRUCTURE;
    uint64_t fn_bits=(uint64_t)final->site_count+parameters+3;
    uint64_t cell_bits=(uint64_t)parameters+3;
    uint64_t origin_words=(fn_bits+63)/64,cell_words=(cell_bits+63)/64;
    uint64_t fn_words=origin_words+(uint64_t)final->binding_count*(origin_words+cell_words);
    if (fn_bits>UINT32_MAX || cell_bits>UINT32_MAX ||
        fn_words>UINT32_MAX || cell_words>UINT32_MAX ||
        fn_words+cell_words>UINT32_MAX) return XR_XIR_BUDGET;
    if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
    *output=(EffectInvocationBasis){parameters,(uint32_t)origin_words,(uint32_t)fn_words,
        (uint32_t)cell_words,(uint32_t)(fn_words+cell_words)};
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_basis_parameters(const EffectInvocationWorkspace *owner,
    uint32_t parameters,EffectInvocationBasis *output) {
    if (!owner) return XR_XIR_BAD_STRUCTURE;
    return effect_invocation_basis_parameters_read(owner->work,&owner->final,parameters,output);
}

static XrXirStatus effect_invocation_basis(const EffectInvocationWorkspace *owner,
    uint32_t root,EffectInvocationBasis *output) {
    if (!owner || !effect_body_material_shape(owner->final.material) || root>=owner->body.domain.function_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionDimensions head={0};XrXirStatus status=effect_invocation_dimensions(owner,root,&head);
    return status==XR_XIR_OK?effect_invocation_basis_parameters(owner,head.parameter_count,output):status;
}

static void effect_invocation_final_clear(EffectInvocationFinal *final) {
    if (!final) return;
    effect_invocation_result_clear(&final->summary);
    for (uint32_t n=0;n<final->count;++n) {
        xr_compile_resources_free(final->nodes[n].causes);
        xr_compile_resources_free(final->nodes[n].producers);
        xr_compile_resources_free(final->nodes[n].classes);
    }
    for (uint32_t d=0;d<final->deferred_count;++d)
        xr_compile_resources_free(final->deferred_calls[d].arguments);
    xr_compile_resources_free(final->deferred_calls);
    xr_compile_resources_free(final->nodes);xr_compile_resources_free(final->edges);
    xr_compile_resources_free(final->sites);
    xr_compile_resources_free(final->roots);xr_compile_resources_free(final->root_spaces);
    /* Local seeds, physical rows and Cell proof share the material lifetime. */
}

static void effect_invocation_final_free(EffectInvocationFinal *final) {
    if (!final) return;
    effect_invocation_final_clear(final);xr_compile_resources_free(final);
}

static void effect_invocation_free(EffectInvocationWorkspace *owner) {
    if (!owner) return;
    effect_invocation_scratch_free(owner);
    effect_invocation_result_free(owner->prepared_result);
    effect_invocation_final_clear(&owner->final);
    xr_compile_resources_free(owner->edge_index);xr_compile_resources_free(owner->consumers);
    xr_compile_resources_free(owner);
}

/* The formula graph's queue is shared by every invocation equation. All
 * live entries retain source order during growth; no second queue solves ROOT. */
static XrXirStatus effect_invocation_capacity(EffectInvocationWorkspace *owner) {
    if (!owner || !owner->graph || !owner->work ||
        (owner->final.count && (!owner->final.nodes || !owner->consumers || !owner->states)))
        return XR_XIR_BAD_STRUCTURE;
    if (owner->final.count<owner->final.capacity) return XR_XIR_OK;
    if (owner->final.count>=owner->maximum) return XR_XIR_BUDGET;
    uint32_t capacity=owner->final.capacity ? owner->final.capacity : 8;
    if (capacity>owner->maximum) capacity=owner->maximum;
    else if (owner->final.capacity) capacity=capacity>owner->maximum/2 ?
        owner->maximum : capacity*2;
    uint64_t node_bytes=(uint64_t)capacity*sizeof(*owner->final.nodes);
    uint64_t index_bytes=(uint64_t)capacity*(sizeof(*owner->consumers)+sizeof(*owner->states));
    if (capacity<=owner->final.count || node_bytes>SIZE_MAX ||
        index_bytes>SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    /* Each admitted slot receives a complete value in intern. Unused capacity
     * has no initialized node and is never visited by lookup or destruction. */
    EffectInvocationNode *nodes=xir_compile_alloc(owner->work,(size_t)node_bytes,&status);
    /* One scratch allocation owns both the reverse index and its state tail.
     * Only the base is freed; neither view is transferred into Final. */
    uint32_t *consumers=status==XR_XIR_OK ?
        xir_compile_alloc(owner->work,(size_t)index_bytes,&status) : NULL;
    EffectInvocationState *states=consumers ? (EffectInvocationState *)(consumers+capacity) : NULL;
    uint32_t *queue=status==XR_XIR_OK ?
        xir_compile_alloc(owner->work,(size_t)capacity*sizeof(*queue),&status) : NULL;
    if (!nodes || !consumers || !queue) {
        xr_compile_resources_free(nodes);xr_compile_resources_free(consumers);
        xr_compile_resources_free(queue);return status;
    }
    uint64_t work=(uint64_t)owner->final.count*(sizeof(*nodes)+sizeof(*consumers)+sizeof(*states))+
        (uint64_t)owner->pending*sizeof(*queue)+(uint64_t)capacity*3;
    if (!xir_compile_work(owner->work,work)) {
        xr_compile_resources_free(nodes);xr_compile_resources_free(consumers);
        xr_compile_resources_free(queue);return XR_XIR_BUDGET;
    }
    if (owner->final.count) {
        memcpy(nodes,owner->final.nodes,(size_t)owner->final.count*sizeof(*nodes));
        memcpy(consumers,owner->consumers,(size_t)owner->final.count*sizeof(*consumers));
        memcpy(states,owner->states,(size_t)owner->final.count*sizeof(*states));
    }
    for (uint32_t n=owner->final.count;n<capacity;++n) {
        consumers[n]=UINT32_MAX;states[n]=(EffectInvocationState){0};
    }
    for (uint32_t q=0;q<owner->pending;++q) {
        uint64_t at=(uint64_t)owner->front+q;
        if (at>=owner->final.capacity) at-=owner->final.capacity;
        queue[q]=owner->graph->queue[at];
    }
    xr_compile_resources_free(owner->final.nodes);xr_compile_resources_free(owner->consumers);
    xr_compile_resources_free(owner->graph->queue);
    owner->final.nodes=nodes;owner->consumers=consumers;owner->states=states;owner->graph->queue=queue;
    owner->final.capacity=capacity;owner->front=0;
    owner->back=owner->pending==capacity ? 0 : owner->pending;
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_enqueue(EffectInvocationWorkspace *owner,uint32_t node) {
    if (node>=owner->final.count || !owner->states) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
    if (owner->states[node].queued) return XR_XIR_OK;
    if (owner->pending>=owner->final.capacity) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
    owner->graph->queue[owner->back]=node;
    owner->back=owner->back+1==owner->final.capacity ? 0 : owner->back+1;
    ++owner->pending;owner->states[node].queued=true;return XR_XIR_OK;
}

/* Neither a producer pointer nor a numeric target alone is a key. Exact
 * physical origin rows are copied into this owner before the node is linked. */
static XrXirStatus effect_invocation_intern(EffectInvocationWorkspace *owner,
    uint32_t root,uint32_t body,const uint64_t *input,uint32_t *output) {
    if (body>=owner->body.domain.function_count || !output) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationBasis basis={0};XrXirStatus status=effect_invocation_basis(owner,root,&basis);
    if (status!=XR_XIR_OK) return status;
    EffectBodyFunctionDimensions head={0};status=effect_invocation_dimensions(owner,body,&head);
    if (status!=XR_XIR_OK) return status;
    uint32_t parameters=head.parameter_count;
    uint64_t words=(uint64_t)parameters*basis.row_words;
    if (words>SIZE_MAX/sizeof(*input) || (words && !input)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t n=0;n<owner->final.count;++n) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&owner->final.nodes[n];
        if (node->root!=root || node->body!=body || node->parameter_count!=parameters) continue;
        bool same=true;
        for (size_t w=0;w<(size_t)words;++w) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            if (node->input[w]!=input[w]) same=false;
        }
        if (same) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            if (node->instruction_count!=head.instruction_count) return XR_XIR_BAD_STRUCTURE;
            *output=n;return XR_XIR_OK;
        }
    }
    status=effect_invocation_capacity(owner);
    if (status!=XR_XIR_OK) return status;
    uint32_t dependency_words=(uint32_t)(((uint64_t)basis.parameters+63)/64);
    uint32_t context_words=(uint32_t)(((uint64_t)head.instruction_count+63)/64);
    uint64_t output_words=(uint64_t)dependency_words*2+context_words;
    uint64_t cause_count=(uint64_t)basis.parameters*2+3;
    if (output_words>SIZE_MAX/(2*sizeof(uint64_t)) || cause_count>UINT32_MAX ||
        cause_count>SIZE_MAX/(2*sizeof(EffectInvocationCause))) return XR_XIR_BUDGET;
    size_t input_bytes=(size_t)words*sizeof(uint64_t);
    size_t output_bytes=(size_t)output_words*2*sizeof(uint64_t);
    size_t cause_bytes=(size_t)cause_count*2*sizeof(EffectInvocationCause);
    if (input_bytes>SIZE_MAX-cause_bytes || output_bytes>SIZE_MAX-cause_bytes-input_bytes)
        return XR_XIR_BUDGET;
    uint64_t coverage_words=parameters;
    if (coverage_words>SIZE_MAX/sizeof(uint64_t)) return XR_XIR_BUDGET;
    size_t coverage_bytes=(size_t)coverage_words*sizeof(uint64_t);
    if (coverage_bytes>SIZE_MAX-cause_bytes-input_bytes-output_bytes) return XR_XIR_BUDGET;
    /* These rows share exactly one equation lifetime. The cause pointer is
     * the allocation root; every input and output pointer is an interior view. */
    EffectInvocationCause *causes=xir_compile_calloc(owner->work,1,
        cause_bytes+input_bytes+output_bytes+coverage_bytes,&status);
    if (!causes) return status;
    uint8_t *memory=(uint8_t *)causes;
    uint64_t *owned=words ? (uint64_t *)(memory+cause_bytes) : NULL;
    uint64_t *outputs=output_words ? (uint64_t *)(memory+cause_bytes+input_bytes) : NULL;
    uint64_t *local_cells=outputs ? outputs+(size_t)output_words : NULL;
    uint64_t *coverage=parameters ?
        (uint64_t *)(memory+cause_bytes+input_bytes+output_bytes) : NULL;
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
    EffectInvocationClassFact *classes=NULL; uint32_t class_count=0;
    status=effect_invocation_class_storage(owner,body,&classes,&class_count);
    if (status!=XR_XIR_OK) { xr_compile_resources_free(causes);return status; }
    uint32_t at=owner->final.count;
    owner->final.nodes[at]=(EffectInvocationNode){.root=root,.body=body,
        .parameter_count=parameters,.instruction_count=head.instruction_count,
        .input=owned,.cells=outputs,
        .causes=causes,.cause_count=(uint32_t)cause_count,.local_cells=local_cells,
        .parameter_coverage=coverage,.edge_head=UINT32_MAX,.classes=classes,.class_count=class_count};
    ++owner->final.count;
    status=effect_invocation_enqueue(owner,at);
    if (status!=XR_XIR_OK) return status;
    *output=at;return XR_XIR_OK;
}

#include "xxir_effect_invocation_edge_index.inc.c"

static XrXirStatus effect_invocation_edge(EffectInvocationWorkspace *owner,
    uint32_t caller,uint32_t target,uint32_t instruction,uint32_t producer,bool latent) {
    if (caller>=owner->final.count || target>=owner->final.count) return XR_XIR_BAD_STRUCTURE;
    /* This numeric span was copied from the actual owned head in intern.
     * Independent closed/seal and every runtime node_shape still compare
     * it with this generation's complete immutable Material. */
    if (!xir_compile_work(owner->work,sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t instructions=owner->final.nodes[caller].instruction_count;
    XrXirStatus read_status=XR_XIR_OK;
    if (owner->final.nodes[caller].root!=owner->final.nodes[target].root ||
        instruction>=instructions ||
        (producer!=UINT32_MAX && (producer>=owner->final.site_count ||
            owner->final.sites[producer].target!=owner->final.nodes[target].body)))
        return XR_XIR_BAD_STRUCTURE;
    if (!!owner->edge_index!=!!owner->edge_index_capacity ||
        (owner->final.edge_count && !owner->edge_index)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,sizeof(EffectInvocationEdgeKey)+2*sizeof(size_t)+3))
        return XR_XIR_BUDGET;
    EffectInvocationEdgeKey key={caller,target,instruction,producer,(uint32_t)latent};
    size_t position=0;
    if (owner->edge_index) {
        read_status=effect_invocation_edge_slot(owner,owner->edge_index,
            owner->edge_index_capacity,&key,&position);
        if (read_status!=XR_XIR_OK) return read_status;
        if (owner->edge_index[position]) return XR_XIR_OK;
    }
    size_t previous_capacity=owner->edge_index_capacity;
    read_status=effect_invocation_edge_index_reserve(owner);
    if (read_status!=XR_XIR_OK) return read_status;
    if (previous_capacity!=owner->edge_index_capacity) {
        read_status=effect_invocation_edge_slot(owner,owner->edge_index,
            owner->edge_index_capacity,&key,&position);
        if (read_status!=XR_XIR_OK) return read_status;
        if (owner->edge_index[position]) return XR_XIR_BAD_STRUCTURE;
    }
    if (owner->final.edge_count==owner->final.edge_capacity) {
        uint32_t maximum=owner->maximum;
        if ((uint64_t)maximum>SIZE_MAX/sizeof(*owner->final.edges)) return XR_XIR_BUDGET;
        uint32_t capacity=owner->final.edge_capacity ? owner->final.edge_capacity : 8;
        if (capacity>maximum) capacity=maximum;
        else if (owner->final.edge_capacity) capacity=capacity>maximum/2 ? maximum : capacity*2;
        if (capacity<=owner->final.edge_count) return XR_XIR_BUDGET;
        XrXirStatus status=XR_XIR_OK;
        EffectInvocationEdge *edges=xir_compile_alloc(owner->work,
            (size_t)capacity*sizeof(*edges),&status);
        if (!edges) return status;
        if (!xir_compile_work(owner->work,(uint64_t)owner->final.edge_count*sizeof(*edges))) {
            xr_compile_resources_free(edges);return XR_XIR_BUDGET;
        }
        if (owner->final.edge_count) memcpy(edges,owner->final.edges,(size_t)owner->final.edge_count*sizeof(*edges));
        xr_compile_resources_free(owner->final.edges);owner->final.edges=edges;owner->final.edge_capacity=capacity;
    }
    if (!xir_compile_work(owner->work,sizeof(EffectInvocationEdge)+2+sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t at=owner->final.edge_count;
    owner->final.edges[at]=(EffectInvocationEdge){caller,target,instruction,producer,
        owner->final.nodes[caller].edge_head,owner->consumers[target],latent};
    owner->final.nodes[caller].edge_head=at;owner->consumers[target]=at;
    ++owner->final.edge_count;owner->edge_index[position]=at+1;
    /* A completed child's producer can predate a newly linked real edge. */
    return owner->producer_enabled || owner->class_enabled ? effect_invocation_enqueue(owner,caller) : XR_XIR_OK;
}

static XrXirStatus effect_invocation_join(EffectInvocationWorkspace *owner,
    uint32_t caller,uint32_t target,uint32_t instruction,bool *changed) {
    if (caller>=owner->final.count || target>=owner->final.count || !changed ||
        owner->final.nodes[caller].root!=owner->final.nodes[target].root) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationNode *to=&owner->final.nodes[caller];
    const EffectInvocationNode *from=&owner->final.nodes[target];
    EffectInvocationBasis basis={0};XrXirStatus status=effect_invocation_basis(owner,to->root,&basis);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
    uint32_t mask=to->intrinsic_mask|from->intrinsic_mask;
    bool deferred=to->deferred||from->deferred;
    if (mask!=to->intrinsic_mask || deferred!=to->deferred) *changed=true;
    to->intrinsic_mask=mask;to->deferred=deferred;
    if (from->deferred) {
        EffectBodyFunctionDimensions head={0};status=effect_invocation_dimensions(owner,to->body,&head);
        if (status!=XR_XIR_OK) return status;
        uint32_t instructions=head.instruction_count;
        if (instruction>=instructions || !effect_invocation_node_row(to,EFFECT_INVOCATION_CONTEXTS,false)) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        uint64_t bit=UINT64_C(1)<<(instruction%64);
        if (!(effect_invocation_node_row(to,EFFECT_INVOCATION_CONTEXTS,false)[instruction/64]&bit)) *changed=true;
        effect_invocation_node_row(to,EFFECT_INVOCATION_CONTEXTS,false)[instruction/64]|=bit;
    }
    uint32_t words=(uint32_t)(((uint64_t)basis.parameters+63)/64);
    uint64_t *to_parameters=effect_invocation_node_row(to,EFFECT_INVOCATION_PARAMETERS,false);
    const uint64_t *from_parameters=effect_invocation_node_row(from,EFFECT_INVOCATION_PARAMETERS,false);
    for (uint32_t w=0;w<words;++w) {
        if (!xir_compile_work(owner->work,6)) return XR_XIR_BUDGET;
        uint64_t cells=to->cells[w]|from->cells[w];
        uint64_t parameters=to_parameters[w]|from_parameters[w];
        if (cells!=to->cells[w] || parameters!=to_parameters[w]) *changed=true;
        to->cells[w]=cells;to_parameters[w]=parameters;
    }
    return XR_XIR_OK;
}
