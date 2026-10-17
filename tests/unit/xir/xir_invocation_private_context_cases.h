/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_private_context_cases.h - Private execution owner retirement
 */
#ifndef XIR_INVOCATION_PRIVATE_CONTEXT_CASES_H
#define XIR_INVOCATION_PRIVATE_CONTEXT_CASES_H
typedef struct PrivateContextInput {
    EffectBodyMaterial body;
    EffectInvocationFinal final;
    EffectBodyFunction heads[8];EffectBodyRun runs[8][8];
    XrXirType parameters[8][4];XrXirBlock blocks[8][4];uint32_t operands[8][8];char names[8][32];
    XrXirTypeNode types[32];EffectInvocationNode nodes[32];EffectInvocationEdge edges[64];
    unsigned char payload[32][2048];size_t payload_bytes[32];
    EffectInvocationProducer producers[32][32];EffectInvocationClassFact classes[32][32];
} PrivateContextInput;
static void private_context_capture(PrivateContextInput *copy,const EffectInvocationCertificate *certificate) {
    const EffectBodyMaterial *body=certificate->body;const EffectInvocationFinal *final=certificate->final;
    CHECK(effect_body_count(body)<=8 && body->terms.types.count<=32 && final->count<=32 && final->edge_count<=64);
    copy->body=*body;copy->final=*final;
    memcpy(copy->heads,body->functions,(size_t)effect_body_count(body)*sizeof(*body->functions));
    if(body->terms.types.count)memcpy(copy->types,body->terms.types.nodes,(size_t)body->terms.types.count*sizeof(*copy->types));
    memcpy(copy->nodes,final->nodes,(size_t)final->count*sizeof(*copy->nodes));
    if(final->edge_count)memcpy(copy->edges,final->edges,(size_t)final->edge_count*sizeof(*copy->edges));
    for(uint32_t f=0;f<effect_body_count(body);++f){const EffectBodyFunction *head=&body->functions[f];
        CHECK(head->run_count<=8 && head->parameter_count<=4 && head->block_count<=4 && head->operand_count<=8 && head->name_length<=32);
        if(head->run_count)memcpy(copy->runs[f],head->runs,(size_t)head->run_count*sizeof(*head->runs));
        if(head->parameter_count)memcpy(copy->parameters[f],head->parameters,(size_t)head->parameter_count*sizeof(*head->parameters));
        if(head->block_count)memcpy(copy->blocks[f],head->blocks,(size_t)head->block_count*sizeof(*head->blocks));
        if(head->operand_count)memcpy(copy->operands[f],head->operands,(size_t)head->operand_count*sizeof(*head->operands));
        if(head->name_length)memcpy(copy->names[f],head->name,head->name_length);
    }
    for(uint32_t n=0;n<final->count;++n){const EffectInvocationNode *node=&final->nodes[n];
        copy->payload_bytes[n]=driver_lookup_block_bytes(node->causes)-sizeof(CompileAllocation);
        CHECK(copy->payload_bytes[n]<=2048 && node->producer_count<=32 && node->class_count<=32);
        memcpy(copy->payload[n],node->causes,copy->payload_bytes[n]);
        if(node->producer_count)memcpy(copy->producers[n],node->producers,(size_t)node->producer_count*sizeof(*node->producers));
        if(node->class_count)memcpy(copy->classes[n],node->classes,(size_t)node->class_count*sizeof(*node->classes));
    }
}
static void private_context_input_same(const PrivateContextInput *copy,const EffectInvocationCertificate *certificate) {
    const EffectBodyMaterial *body=certificate->body;const EffectInvocationFinal *final=certificate->final;
    EffectBodyMaterial header=*body;CHECK(header.references);header.references=copy->body.references;
    CHECK(!memcmp(&header,&copy->body,sizeof(header)) && !memcmp(final,&copy->final,sizeof(*final)) &&
        !memcmp(body->functions,copy->heads,(size_t)effect_body_count(body)*sizeof(*body->functions)));
    CHECK(!body->terms.types.count || !memcmp(body->terms.types.nodes,copy->types,(size_t)body->terms.types.count*sizeof(*copy->types)));
    CHECK(!memcmp(final->nodes,copy->nodes,(size_t)final->count*sizeof(*final->nodes)));
    CHECK(!final->edge_count || !memcmp(final->edges,copy->edges,(size_t)final->edge_count*sizeof(*final->edges)));
    for(uint32_t f=0;f<effect_body_count(body);++f){const EffectBodyFunction *head=&body->functions[f];
        CHECK(!head->run_count || !memcmp(head->runs,copy->runs[f],(size_t)head->run_count*sizeof(*head->runs)));
        CHECK(!head->parameter_count || !memcmp(head->parameters,copy->parameters[f],(size_t)head->parameter_count*sizeof(*head->parameters)));
        CHECK(!head->block_count || !memcmp(head->blocks,copy->blocks[f],(size_t)head->block_count*sizeof(*head->blocks)));
        CHECK(!head->operand_count || !memcmp(head->operands,copy->operands[f],(size_t)head->operand_count*sizeof(*head->operands)));
        CHECK(!head->name_length || !memcmp(head->name,copy->names[f],head->name_length));
    }
    for(uint32_t n=0;n<final->count;++n){const EffectInvocationNode *node=&final->nodes[n];
        CHECK(!memcmp(node->causes,copy->payload[n],copy->payload_bytes[n]));
        CHECK(!node->producer_count || !memcmp(node->producers,copy->producers[n],(size_t)node->producer_count*sizeof(*node->producers)));
        CHECK(!node->class_count || !memcmp(node->classes,copy->classes[n],(size_t)node->class_count*sizeof(*node->classes)));
    }
}
static void *private_context_old_blocks[4];
static unsigned private_context_dead;
static bool private_context_expect_retirement,private_context_sampling;
static XrXirEffects *private_context_effects;
static const PrivateContextInput *private_context_input;
static uint64_t private_context_entry,private_context_allocation_work[2048];
static size_t private_context_allocations,private_context_census_sites;
static bool private_context_attempted;
static void private_context_release(void *raw) {
    for(unsigned i=0;i<4;++i)if(raw==private_context_old_blocks[i])private_context_dead|=1u<<i;
}
static void private_context_allocate(void *raw) {
    (void)raw;CHECK(private_context_effects && private_context_input && private_context_allocations<2048);
    if(!private_context_allocations){
        CHECK(private_context_dead==(private_context_expect_retirement?15u:0u));
        CHECK((private_context_effects->contexts==NULL)==(private_context_expect_retirement || !private_context_old_blocks[0]));
        private_context_input_same(private_context_input,private_context_effects->invocations);
    }
    if(private_context_sampling)private_context_allocation_work[private_context_allocations]=private_context_effects->resources->stats.work;
    ++private_context_allocations;
}
static void private_context_observe(XrXirEffects *effects,const PrivateContextInput *input,bool retire) {
    CHECK(!rp_allocation_observer && !rp_release_observer);EffectContextOwner *old=effects->contexts;
    private_context_old_blocks[0]=old?(CompileAllocation *)old-1:NULL;
    private_context_old_blocks[1]=old?(CompileAllocation *)old->ordinary-1:NULL;
    private_context_old_blocks[2]=old?(CompileAllocation *)old->dense-1:NULL;
    private_context_old_blocks[3]=old?(CompileAllocation *)old->forest-1:NULL;
    private_context_dead=0;private_context_allocations=0;private_context_effects=effects;private_context_input=input;
    private_context_expect_retirement=retire;rp_release_observer=private_context_release;rp_allocation_observer=private_context_allocate;
}
static void private_context_observe_close(void) {
    rp_release_observer=NULL;rp_allocation_observer=NULL;private_context_effects=NULL;private_context_input=NULL;
}
static void private_context_guards(const XrXirCompileContext *context,const XrXirModule *source,XrXirEffects *effects) {
    XrCompileResourceStats before=rp_stats(context);RootParameterMark mark=rp_mark();size_t attempts=rp_attempts;
    EffectContextOwner *old=effects->contexts;EffectInvocationCertificate *invocations=effects->invocations;
    CHECK(effect_context_owner_private_refresh(NULL,source,effects)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_context_owner_private_refresh(context,NULL,effects)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_context_owner_private_refresh(context,source,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirModule malformed=*source;++malformed.function_count;
    CHECK(effect_context_owner_private_refresh(context,&malformed,effects)==XR_XIR_BAD_STRUCTURE);
    effects->invocations=NULL;CHECK(effect_context_owner_private_refresh(context,source,effects)==XR_XIR_BAD_STRUCTURE);effects->invocations=invocations;
    EffectBodyMaterial *body=invocations->body;invocations->body=NULL;
    CHECK(effect_context_owner_private_refresh(context,source,effects)==XR_XIR_BAD_STRUCTURE);invocations->body=body;
    XrCompileResourceStats after=rp_stats(context);CHECK(!memcmp(&before,&after,sizeof(before)) && effects->contexts==old && rp_attempts==attempts);rp_balanced(mark);
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats other=rp_stats(&foreign);
    CHECK(effect_context_owner_private_refresh(&foreign,source,effects)==XR_XIR_BAD_STRUCTURE && effects->contexts==old);
    after=rp_stats(&foreign);CHECK(!memcmp(&other,&after,sizeof(other)));rp_owner_free(&foreign,other.live_bytes);rp_balanced(mark);
    malformed=*source;malformed.provenance=NULL;before=rp_stats(context);
    CHECK(effect_context_owner_private_refresh(context,&malformed,effects)==XR_XIR_OK && effects->contexts==old);
    after=rp_stats(context);CHECK(!memcmp(&before,&after,sizeof(before)));rp_balanced(mark);
}
static XrXirStatus private_context_proof(const XrXirCompileContext *context,XrXirEffects *effects,const XrXirModule *source) {
    EffectInvocationCertificate *certificate=effects->invocations;
    XrXirStatus status=effect_invocation_current_input_match(context,source,certificate->body);
    if(status==XR_XIR_OK)status=effect_invocation_publication_match(context,certificate,&certificate->final->summary);
    EffectInvocationWorkspace read={0};EffectBodyRequest body=effect_body_owned_request(context,certificate->body);
    if(status==XR_XIR_OK)status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&read);
    if(status==XR_XIR_OK)status=effect_invocation_class_verify_read(&read,&body);effect_invocation_read_close(&read);return status;
}
static XrXirStatus private_context_run(const XrXirCompileContext *context,unsigned mode,size_t inject,uint64_t cut) {
    private_context_attempted=false;RootParameterBindingFixture fixture;rp_binding_fixture(&fixture,context,false);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_analyze(checked,&effects);
    const XrXirModule *source=checked?xr_xir_compile_artifact_module(checked):NULL;
    if(status!=XR_XIR_OK){xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);return status;}
    CHECK(effects->contexts && effects->contexts->ordinary && effects->contexts->dense && effects->contexts->forest &&
        source->provenance && source->provenance->kind==XR_XIR_EVIDENCE_TEMPLATE);
    status=private_context_proof(context,effects,source);
    if(status!=XR_XIR_OK){xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);return status;}
    if(mode==2)private_context_guards(context,source,effects);
    PrivateContextInput input;private_context_capture(&input,effects->invocations);
    EffectContextOwner *old=effects->contexts;uint32_t forest_count=old->forest->count;CHECK(forest_count<=16);
    XrXirRootEffects facts[16];XrXirRootEffectWitness witnesses[32];
    memcpy(facts,old->forest->facts,(size_t)forest_count*sizeof(*facts));
    memcpy(witnesses,old->forest->witnesses,(size_t)forest_count*2*sizeof(*witnesses));
    if(mode==3){effects->contexts=NULL;effect_context_owner_free(old);private_context_input_same(&input,effects->invocations);old=NULL;}
    uint64_t entry=rp_stats(context).work;size_t attempts=rp_attempts;
    if(private_context_sampling)private_context_entry=entry;
    if(cut!=UINT64_MAX)context->resources->limits.work=entry+cut;
    private_context_observe(effects,&input,mode!=1 && old!=NULL);private_context_attempted=true;
    if(inject!=SIZE_MAX){rp_fail_at=attempts+inject;rp_injected=false;}
    if(mode==1){rp_fail_at=attempts;rp_injected=false;
        status=effect_context_owner_refresh(context,source,effects,effects->invocations->body);rp_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && effects->contexts==old && !private_context_dead && !private_context_allocations);
        private_context_input_same(&input,effects->invocations);
        status=effect_context_owner_refresh(context,source,effects,effects->invocations->body);
    }else status=effect_context_owner_private_refresh(context,source,effects);
    rp_fail_at=SIZE_MAX;private_context_observe_close();private_context_input_same(&input,effects->invocations);
    if(cut!=UINT64_MAX)CHECK(status==XR_XIR_BUDGET);
    if(private_context_sampling)private_context_census_sites=private_context_allocations;
    if(status==XR_XIR_OK){CHECK(effects->contexts && private_context_dead==(old?15u:0u));
        status=private_context_proof(context,effects,source);if(status!=XR_XIR_OK)goto done;
        CHECK(effects->contexts->forest->count==forest_count);
        for(uint32_t i=0;i<forest_count;++i){CHECK(seed_cursor_equal(facts[i],effects->contexts->forest->facts[i]));
            CHECK(effect_context_witness_same(&witnesses[i],&effects->contexts->forest->witnesses[i]) &&
                effect_context_witness_same(&witnesses[forest_count+i],&effects->contexts->forest->witnesses[forest_count+i]));}
        xr_xir_compile_artifact_free(checked);checked=NULL;memset(&fixture,0xCE,sizeof(fixture));
        EffectContextOwner *received=effects->contexts;effects->contexts=NULL;xr_xir_compile_effects_free(effects);effects=NULL;
        EffectInvocationCertificate *retained=received->forest->invocations;
        status=effect_invocation_certificate_result_match(context,retained,&retained->final->summary);
        if(status==XR_XIR_OK)status=effect_invocation_closed_read(context,retained->final);
        XrXirRootCauseTrace *trace=NULL;
        if(status==XR_XIR_OK)status=effect_context_forest_trace(context,received->forest,1,&trace);
        if(status==XR_XIR_OK){const XrXirRootEffects *actual=xr_xir_root_cause_trace_facts(trace);
            CHECK(actual && !actual->requires_root && actual->unresolved);}
        xr_xir_compile_root_cause_trace_free(trace);effect_context_owner_free(received);
    }else{CHECK(mode!=1 && (status==XR_XIR_OUT_OF_MEMORY || status==XR_XIR_BUDGET));
        if(inject!=SIZE_MAX)CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts==attempts+inject+1);
        if(cut!=UINT64_MAX){CHECK(status==XR_XIR_BUDGET && context->resources->limits.work==entry+cut);
            CHECK(effects->contexts==(cut<2*sizeof(effects->contexts)?old:NULL));
            if(cut<=2*sizeof(effects->contexts)){RootParameterMark held=rp_mark();uint64_t work=rp_stats(context).work;
                CHECK(effect_context_owner_private_refresh(context,source,effects)==XR_XIR_BUDGET && rp_stats(context).work==work);rp_balanced(held);}}
        else CHECK(!effects->contexts && private_context_dead==15 && private_context_proof(context,effects,source)==XR_XIR_OK);
    }
done:
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);return status;
}
static void invocation_private_context_controls(void) {
    RootParameterMark mark=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
    private_context_sampling=true;CHECK(private_context_run(&context,0,SIZE_MAX,UINT64_MAX)==XR_XIR_OK);private_context_sampling=false;
    CHECK(private_context_census_sites && private_context_census_sites<=2048);size_t sites=private_context_census_sites;
    uint64_t cuts[2048];for(size_t i=0;i<sites;++i)cuts[i]=private_context_allocation_work[i]-private_context_entry;
    XrCompileResourceStats measured=rp_stats(&context);rp_owner_free(&context,live);rp_balanced(mark);
    for(unsigned mode=1;mode<=3;++mode){context=rp_owner(rp_caps());live=rp_stats(&context).live_bytes;
        CHECK(private_context_run(&context,mode,SIZE_MAX,UINT64_MAX)==XR_XIR_OK);rp_owner_free(&context,live);rp_balanced(mark);}
    for(uint64_t cut=0;cut<=2*sizeof(void *);++cut){context=rp_owner(rp_caps());live=rp_stats(&context).live_bytes;
        CHECK(private_context_run(&context,0,SIZE_MAX,cut)==XR_XIR_BUDGET && private_context_attempted);rp_owner_free(&context,live);rp_balanced(mark);}
    for(size_t at=0;at<sites;++at)for(unsigned work=0;work<2;++work){context=rp_owner(rp_caps());live=rp_stats(&context).live_bytes;
        CHECK(private_context_run(&context,0,work?SIZE_MAX:at,work?cuts[at]:UINT64_MAX)==(work?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY) && private_context_attempted);
        rp_owner_free(&context,live);rp_balanced(mark);}
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        context=rp_owner(caps);live=rp_stats(&context).live_bytes;
        CHECK(private_context_run(&context,0,SIZE_MAX,UINT64_MAX)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,live);rp_balanced(mark);}
    fprintf(stderr,"Private context lifetime: genuine checked TEMPLATE and current Input, all four old owner headers die before first fresh allocation, complete header/node/edge/cause/input/output/type/name/parameter/block/operand/run bytes unchanged, original public refresh first OOM and same-owner retry, no-old/non-template and seven guards, every retirement unit0..16 and same finite owner without cap reset; every actual%zu constructor OOM and allocation-admitted work refusal with live Input and physical cleanup, fresh current correspondence/publication/Class and identical complete forest facts/witnesses, independent retained forest after producer/Checked/top effects death; six axes%llu/%llu/%llu exact/minusone physical0 PASS\n",
        sites,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_PRIVATE_CONTEXT_CASES_H
