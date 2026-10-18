/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_final_view.inc.c - Fresh views of admitted finite origins
 *
 * KEY CONCEPT:
 *   Receiving scratch projects Final without an origin solver or discovery.
 *   Only bounded scalar dimensions survive allocation. Actual owned input
 *   heads and the selected key are framed after the allocator callback.
 */
typedef struct EffectInvocationViewLayout {
    EffectInvocationBasis basis;
    uint32_t body,root,values,rows,parameters,instructions,sites,bindings;
} EffectInvocationViewLayout;

/* These untrusted dimensions size bounded scratch only. No input head is
 * borrowed or permission accepted before the allocator callback returns. */
static XrXirStatus effect_invocation_final_layout(const EffectInvocationFlow *flow,
    EffectInvocationViewLayout *output) {
    if(!flow || flow->owner || !flow->read || !output) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationRead *owner=flow->read;const EffectInvocationFinal *final=owner->final;
    if(!final || !owner->roles.sealed || owner->roles.construction || owner->discovery ||
        owner->roles.sealed->final!=final) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(owner->work,final,1,sizeof(*final));if(status!=XR_XIR_OK) return status;
    if(flow->node>=final->count || final->count>final->capacity || !final->physical ||
        !!final->sites!=!!final->site_count) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(owner->work,final->nodes,final->capacity,sizeof(*final->nodes));
    if(status==XR_XIR_OK) status=effect_role_range(owner->work,final->physical,1,sizeof(*final->physical));
    if(status!=XR_XIR_OK) return status;
    if(final->physical->resources!=owner->work->resources) return XR_XIR_BAD_STRUCTURE;
    uint32_t functions=final->physical->count;
    status=effect_role_range(owner->work,final->physical->functions,functions,sizeof(*final->physical->functions));
    if(status==XR_XIR_OK) status=effect_role_range(owner->work,final->roots,functions,sizeof(*final->roots));
    if(status==XR_XIR_OK) status=effect_role_range(owner->work,final->sites,final->site_count,sizeof(*final->sites));
    if(status!=XR_XIR_OK) return status;
    const EffectInvocationNode *node=&final->nodes[flow->node];
    if(node->body>=functions || node->root>=functions || final->roots[node->root]>=final->count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *root=&final->nodes[final->roots[node->root]];
    if(root->body!=node->root) return XR_XIR_BAD_STRUCTURE;
    uint64_t values=(uint64_t)node->parameter_count+node->instruction_count;
    uint32_t rows=final->physical->functions[node->body].origins;
    if(values>UINT32_MAX || rows>values) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationBasis basis={0};
    status=effect_invocation_basis_parameters_read(owner->work,final,root->parameter_count,&basis);
    if(status!=XR_XIR_OK) return status;
    if(!xir_compile_work(owner->work,sizeof(*output)+12)) return XR_XIR_BUDGET;
    *output=(EffectInvocationViewLayout){basis,node->body,node->root,(uint32_t)values,rows,
        node->parameter_count,node->instruction_count,final->site_count,final->binding_count};return XR_XIR_OK;
}

static XrXirStatus effect_invocation_final_head(EffectInvocationFlow *flow,
    const EffectBodyRequest *request) {
    if(!flow || flow->owner || !flow->read || !request) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationRead *owner=flow->read;const EffectInvocationFinal *final=owner->final;
    if(!owner->roles.sealed || owner->roles.construction || owner->discovery || !final ||
        owner->roles.sealed->final!=final) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(owner->work,final,1,sizeof(*final));
    if(status==XR_XIR_OK) status=effect_role_material_frame(owner->work,final->material);
    if(status!=XR_XIR_OK) return status;
    if(!effect_body_request_owned_matches(owner->work,request,final->material) ||
        flow->node>=final->count || final->count>final->capacity ||
        final->physical!=final->material->physical || !final->physical ||
        !!final->sites!=!!final->site_count) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(owner->work,final->nodes,final->capacity,sizeof(*final->nodes));
    if(status==XR_XIR_OK) status=effect_role_range(owner->work,final->material->functions,
        effect_body_count(final->material),sizeof(*final->material->functions));
    if(status==XR_XIR_OK) status=effect_role_range(owner->work,final->physical,1,sizeof(*final->physical));
    if(status==XR_XIR_OK) status=effect_role_range(owner->work,final->sites,final->site_count,sizeof(*final->sites));
    if(status!=XR_XIR_OK) return status;
    if(final->physical->resources!=owner->work->resources ||
        final->physical->count!=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(owner->work,final->physical->functions,final->physical->count,sizeof(*final->physical->functions));
    if(status!=XR_XIR_OK) return status;
    EffectCaptureRead read={.work=owner->work,.final=final,.body=*request};EffectCaptureNode node={0};
    status=effect_capture_node(&read,flow->node,&node);if(status!=XR_XIR_OK) return status;
    const EffectPhysicalFunction *physical=&final->physical->functions[final->nodes[flow->node].body];
    if(physical->parameters!=node.head.parameter_count || physical->instructions!=node.head.instruction_count ||
        physical->values!=node.values || physical->origins>node.values || physical->count>physical->capacity)
        return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(owner->work,physical->records,physical->capacity,sizeof(*physical->records));
    if(status==XR_XIR_OK) status=effect_role_range(owner->work,final->nodes[flow->node].input,
        (uint64_t)node.head.parameter_count*node.basis.row_words,sizeof(uint64_t));
    if(status!=XR_XIR_OK) return status;
    EffectBodyFunctionHead head={0};
    status=effect_body_request_head(request,final->nodes[flow->node].body,&head);
    if(status!=XR_XIR_OK) return status;
    if(!xir_compile_work(owner->work,6)) return XR_XIR_BUDGET;
    if(head.parameters!=node.head.parameters || head.parameter_count!=node.head.parameter_count ||
        head.instruction_count!=node.head.instruction_count || head.operands!=node.head.operands ||
        head.operand_count!=node.head.operand_count || head.result!=node.head.result) return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(owner->work,sizeof(*request)+sizeof(head)+sizeof(node.basis)+
        sizeof(flow->function)+sizeof(flow->physical)+sizeof(flow->values)))
        return XR_XIR_BUDGET;
    flow->body=*request;flow->head=head;flow->function=&flow->head;
    flow->basis=node.basis;flow->values=node.values;flow->physical=physical;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_final_view_facts(EffectInvocationFlow *flow) {
    const EffectInvocationRead *owner=flow->read;const EffectInvocationFinal *final=owner->final;
    if(final->capture.count>final->capture.capacity || !!final->capture.facts!=!!final->capture.capacity)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(owner->work,final->capture.facts,final->capture.capacity,sizeof(*final->capture.facts));
    if(status!=XR_XIR_OK) return status;
    EffectCaptureRead read={.work=owner->work,.final=final,.body=flow->body};
    EffectCaptureFact key={flow->node,0,0,0,0};uint32_t at=0;
    status=effect_capture_lower(&read,&key,&at);if(status!=XR_XIR_OK) return status;
    for(;at<final->capture.count;++at) {
        if(!xir_compile_work(owner->work,6)) return XR_XIR_BUDGET;
        EffectCaptureFact fact=final->capture.facts[at];
        if(fact.node!=flow->node) break;
        if(fact.value>=flow->values) continue;
        if(fact.rank>=final->capture.count) return XR_XIR_BAD_STRUCTURE;
        if(fact.word==EFFECT_CAPTURE_FLAGS) {
            if((uint64_t)fact.bit>EFFECT_CAPTURE_COMPLETE+(uint64_t)final->binding_count) return XR_XIR_BAD_STRUCTURE;
            if(fact.bit==EFFECT_CAPTURE_FIXED) {
                if(!flow->fixed) return XR_XIR_BAD_STRUCTURE;
                if(!xir_compile_work(owner->work,sizeof(uint64_t))) return XR_XIR_BUDGET;
                flow->fixed[fact.value/64]|=UINT64_C(1)<<(fact.value%64);
            }
            continue;
        }
        if(fact.word>=flow->basis.row_words || fact.bit>=64) return XR_XIR_BAD_STRUCTURE;
        if(fact.word>=flow->basis.fn_words) continue;
        uint64_t *row=effect_invocation_row(flow,fact.value);if(!row) return XR_XIR_BAD_STRUCTURE;
        if(!xir_compile_work(owner->work,sizeof(*row))) return XR_XIR_BUDGET;
        row[fact.word]|=UINT64_C(1)<<fact.bit;
    }
    for(uint32_t r=0;r<flow->row_count;++r) {
        bool present=false;
        for(uint32_t w=0;w<flow->basis.origin_words;++w) {
            if(!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            present|=flow->rows[(size_t)r*flow->basis.fn_words+w]!=0;
        }
        if(!present) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_final_view_read(EffectInvocationFlow *flow,
    const EffectBodyRequest *request) {
    if(!flow || request==&flow->body || flow->memory || flow->rows || flow->row_indices || flow->fixed || flow->certified)
        return XR_XIR_BAD_STRUCTURE;
    EffectInvocationViewLayout layout={0};
    XrXirStatus status=effect_invocation_final_layout(flow,&layout);if(status!=XR_XIR_OK) return status;
    EffectInvocationBasis basis=layout.basis;uint32_t values=layout.values,rows=layout.rows;
    const EffectInvocationFinal *final=flow->read->final;
    uint64_t words=(uint64_t)rows*basis.fn_words,indices=rows?(uint64_t)values*sizeof(uint32_t):0;
    uint64_t flags=((uint64_t)values+63)/64;
    if(words>SIZE_MAX/sizeof(uint64_t) || indices>SIZE_MAX-words*sizeof(uint64_t) ||
        words*sizeof(uint64_t)+indices>SIZE_MAX-7) return XR_XIR_BUDGET;
    uint64_t offset=(words*sizeof(uint64_t)+indices+7)&~UINT64_C(7);
    if(flags>(SIZE_MAX-offset)/sizeof(uint64_t)) return XR_XIR_BUDGET;
    size_t bytes=(size_t)(offset+flags*sizeof(uint64_t));
    flow->memory=bytes?xir_compile_alloc(flow->read->work,bytes,&status):NULL;
    if(bytes && !flow->memory) return status;
    status=effect_invocation_final_head(flow,request);if(status!=XR_XIR_OK) return status;
    if(flow->values!=values || flow->physical->origins!=rows || flow->head.parameter_count!=layout.parameters ||
        flow->head.instruction_count!=layout.instructions || flow->basis.parameters!=basis.parameters ||
        flow->basis.fn_words!=basis.fn_words || flow->basis.origin_words!=basis.origin_words ||
        flow->basis.cell_words!=basis.cell_words || flow->basis.row_words!=basis.row_words ||
        flow->read->final!=final || final->site_count!=layout.sites || final->binding_count!=layout.bindings ||
        final->nodes[flow->node].body!=layout.body || final->nodes[flow->node].root!=layout.root)
        return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(flow->read->work,words*sizeof(uint64_t)+indices+flags*sizeof(uint64_t))) return XR_XIR_BUDGET;
    flow->rows=flow->memory;flow->row_count=rows;
    flow->row_indices=rows?(uint32_t *)(flow->rows+(size_t)words):NULL;
    flow->fixed=flags?(uint64_t *)((uint8_t *)flow->memory+(size_t)offset):NULL;
    if(words) memset(flow->rows,0,(size_t)words*sizeof(uint64_t));
    if(indices) memset(flow->row_indices,0xFF,(size_t)indices);
    if(flags) memset(flow->fixed,0,(size_t)flags*sizeof(uint64_t));
    uint32_t row=0;
    for(uint32_t r=0;r<flow->physical->count;++r) {
        if(!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
        EffectPhysicalRecord record=flow->physical->records[r];
        if(!(record.mask&(EFFECT_PHYSICAL_CALLABLE|EFFECT_PHYSICAL_ITERATOR))) continue;
        if(record.value>=values || row>=rows || flow->row_indices[record.value]!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        if(!xir_compile_work(flow->read->work,sizeof(uint32_t))) return XR_XIR_BUDGET;
        flow->row_indices[record.value]=row++;
    }
    if(row!=rows) return XR_XIR_BAD_STRUCTURE;
    status=effect_invocation_final_view_facts(flow);flow->changed=false;return status;
}

static XrXirStatus effect_invocation_final_view(EffectInvocationFlow *flow) {
    if(!flow || !flow->read || !flow->read->final) return XR_XIR_BAD_STRUCTURE;
    return effect_invocation_final_view_read(flow,&flow->read->body);
}
