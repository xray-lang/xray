/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_cause_empty_cases.h - Actual empty cause data and fresh paths
 */
#ifndef XIR_INVOCATION_CAUSE_EMPTY_CASES_H
#define XIR_INVOCATION_CAUSE_EMPTY_CASES_H
static uint64_t cause_empty_records,cause_empty_replay_work;
static EffectInvocationCause cause_empty_literal(uint32_t variant) {
    return (EffectInvocationCause){.witness={variant&1?XR_XIR_ROOT_CAUSE_CALL:XR_XIR_ROOT_CAUSE_NONE,
        variant&2?17:0,variant&4?29:0,variant&8?43:0,variant&16?61:0},
        .next=variant&32?71:UINT32_MAX,.edge=variant&64?83:UINT32_MAX,.distance=variant&128?97:UINT32_MAX};
}
static bool cause_empty_same(const EffectInvocationCause *a,const EffectInvocationCause *b) {
    return effect_invocation_witness_equal(&a->witness,&b->witness) &&
        a->next==b->next && a->edge==b->edge && a->distance==b->distance;
}
static XrXirStatus cause_empty_clear(const XrXirCompileContext *context,EffectInvocationCause *cause) {
    EffectInvocationCause held;memcpy(&held,cause,sizeof(held));size_t attempts=rp_attempts;
    XrCompileResourceStats before=rp_stats(context);XrXirStatus status=effect_invocation_cause_clear(context,cause);
    CHECK(rp_attempts==attempts && rp_stats(context).allocated_bytes==before.allocated_bytes && rp_stats(context).live_bytes==before.live_bytes);
    if(status!=XR_XIR_OK)CHECK(!memcmp(&held,cause,sizeof(held)));return status;
}
/* These eight-field values are physical data, never a permission record. */
static void cause_empty_field_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    const EffectInvocationCause empty=cause_empty_literal(0);
    for(uint32_t variant=0;variant<256;++variant){EffectInvocationCause cause=cause_empty_literal(variant);uint64_t entry=rp_stats(&context).work;
        CHECK(cause_empty_clear(&context,&cause)==XR_XIR_OK && cause_empty_same(&cause,&empty) &&
            rp_stats(&context).work-entry==8+(variant?sizeof(cause):0));
        entry=rp_stats(&context).work;CHECK(cause_empty_clear(&context,&cause)==XR_XIR_OK &&
            cause_empty_same(&cause,&empty) && rp_stats(&context).work-entry==8);
    }
    const uint32_t variants[]={0,1,2,4,8,16,32,64,128,255};uint64_t limit=context.resources->limits.work;
    for(size_t i=0;i<sizeof(variants)/sizeof(*variants);++i){uint64_t exact=8+(variants[i]?sizeof(empty):0);
        for(uint64_t cut=0;cut<=exact;++cut){EffectInvocationCause cause=cause_empty_literal(variants[i]);
            uint64_t entry=rp_stats(&context).work;context.resources->limits.work=entry+cut;
            CHECK(cause_empty_clear(&context,&cause)==(cut==exact?XR_XIR_OK:XR_XIR_BUDGET));
            if(cut<exact)CHECK(cause_empty_clear(&context,&cause)==XR_XIR_BUDGET);
            else CHECK(cause_empty_same(&cause,&empty));context.resources->limits.work=limit;
        }
    }
    EffectInvocationCause cause=cause_empty_literal(255),held=cause;
    CHECK(effect_invocation_cause_clear(NULL,&cause)==XR_XIR_BAD_STRUCTURE && cause_empty_same(&cause,&held));
    CHECK(effect_invocation_cause_clear(&context,NULL)==XR_XIR_BAD_STRUCTURE);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}
/* Copy current genuine facts into private replay scratch. No stack node or
 * recomputed path is published, retained or treated as semantic authority. */
static XrXirStatus cause_empty_replay(const XrXirCompileContext *context,
    const EffectInvocationCertificate *certificate,bool poison) {
    const EffectInvocationFinal *source=certificate->final;CHECK(source->count<=32 && source->capacity<=32);
    EffectInvocationNode nodes[32];EffectInvocationCause original[32][32],causes[32][32];
    EffectInvocationState states[32]={0};uint32_t queue[32]={0},consumers[32];
    for(uint32_t n=0;n<32;++n)consumers[n]=UINT32_MAX;
    for(uint32_t n=0;n<source->count;++n){nodes[n]=source->nodes[n];CHECK(nodes[n].cause_count<=32);
        memcpy(original[n],nodes[n].causes,(size_t)nodes[n].cause_count*sizeof(EffectInvocationCause));
        memcpy(causes[n],original[n],(size_t)nodes[n].cause_count*sizeof(EffectInvocationCause));nodes[n].causes=causes[n];
        uint32_t parameters=certificate->body->functions[nodes[n].root].parameter_count;
        if(poison)for(uint32_t a=0;a<nodes[n].cause_count;++a)if(!effect_invocation_atom(&nodes[n],parameters,a,true))
            causes[n][a]=cause_empty_literal(255);
    }
    for(uint32_t e=0;e<source->edge_count;++e){CHECK(source->edges[e].target<source->count &&
            source->edges[e].consumer_next==consumers[source->edges[e].target]);consumers[source->edges[e].target]=e;}
    EffectGraph graph={.queue=queue};EffectInvocationWorkspace owner={.work=context,.graph=&graph,.states=states,
        .consumers=consumers,.body=effect_body_owned_request(context,certificate->body),.final=*source};owner.final.nodes=nodes;
    RootParameterMark physical=rp_mark();uint64_t live=rp_stats(context).live_bytes;
    XrXirStatus status=effect_invocation_causes(&owner);
    for(uint32_t n=0;n<source->count;++n){CHECK(!memcmp(original[n],source->nodes[n].causes,
            (size_t)nodes[n].cause_count*sizeof(EffectInvocationCause)));
        if(status==XR_XIR_OK)for(uint32_t a=0;a<nodes[n].cause_count;++a){
            CHECK(cause_empty_same(&causes[n][a],&original[n][a]));++cause_empty_records;}
    }
    CHECK(rp_stats(context).live_bytes==live);rp_balanced(physical);return status;
}
static void cause_empty_replay_cuts(const XrXirCompileContext *context,const EffectInvocationCertificate *certificate) {
    uint64_t entry=rp_stats(context).work;CHECK(cause_empty_replay(context,certificate,false)==XR_XIR_OK);
    uint64_t exact=rp_stats(context).work-entry,limit=context->resources->limits.work;CHECK(exact);cause_empty_replay_work=exact;
    for(uint64_t cut=0;cut<=exact;++cut){entry=rp_stats(context).work;CHECK(exact<=limit-entry);
        context->resources->limits.work=entry+cut;
        CHECK(cause_empty_replay(context,certificate,false)==(cut==exact?XR_XIR_OK:XR_XIR_BUDGET));
        if(cut<exact)CHECK(cause_empty_replay(context,certificate,false)==XR_XIR_BUDGET);
        context->resources->limits.work=limit;
    }
}
static XrXirStatus cause_empty_run(const XrXirCompileContext *context,uint32_t mode,bool controls) {
    InvocationClassCellFixture fixture;invocation_class_cell_fixture(&fixture,mode);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;EffectInvocationWorkspace replay={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.source.module,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if(status==XR_XIR_OK){EffectInvocationCertificate *certificate=effects->invocations;CHECK(certificate && certificate->final);
        status=cause_empty_replay(context,certificate,false);
        if(status==XR_XIR_OK && controls){CHECK(cause_empty_replay(context,certificate,true)==XR_XIR_OK);
            if(!mode)cause_empty_replay_cuts(context,certificate);}
        if(status==XR_XIR_OK)status=effect_source_atoms(context,certificate);
        if(status==XR_XIR_OK)status=effect_invocation_closed_read(context,certificate->final);
        if(status==XR_XIR_OK)status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&replay);
        EffectBodyRequest body=effect_body_owned_request(context,certificate->body);
        if(status==XR_XIR_OK)status=effect_invocation_class_verify_read(&replay,&body);
    }
    effect_invocation_read_close(&replay);xr_xir_compile_effects_free(effects);return status;
}
static void invocation_cause_empty_controls(void) {
    cause_empty_field_controls();RootParameterMark physical=rp_mark();XrCompileResourceStats measured={0};
    for(uint32_t mode=0;mode<7;++mode){XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(cause_empty_run(&context,mode,true)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);}
    XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(cause_empty_run(&context,6,false)==XR_XIR_OK);measured=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){XrCompileResourceLimits limits=rp_caps();
        if(!axis)limits.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)limits.live_bytes=measured.peak_bytes-minus;else limits.work=measured.work-minus;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(cause_empty_run(&context,6,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,baseline);rp_balanced(physical);}
    CHECK(cause_empty_records && cause_empty_replay_work);
    fprintf(stderr,"Cause empty values:256 independent eight-field literals and exact canonical8/noncanonical full-store fees, every scalar work unit empty/full/eight individual fields and same finite-owner retry, atomic refusal and no allocation; all7 genuine Class/Cell certificates after Checked/producer death, fresh finite scratch and complete consumer heads from original edges, poisoned nonlocal records reset and %llu complete cause/witness/next/edge/distance comparisons, original certificate bytes unchanged, full replay every work unit through%llu with repeated Budget, independent atom/closure/Class verification; whole genuine-fixture six axes%llu/%llu/%llu exact/minusone PASS\n",
        (unsigned long long)cause_empty_records,(unsigned long long)cause_empty_replay_work,(unsigned long long)measured.allocated_bytes,
        (unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_CAUSE_EMPTY_CASES_H
