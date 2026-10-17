/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_closed_count_cases.h - Current counts in genuine closure
 */
#ifndef XIR_INVOCATION_CLOSED_COUNT_CASES_H
#define XIR_INVOCATION_CLOSED_COUNT_CASES_H
static uint64_t closed_count_values,closed_count_work_units;
static XrXirStatus closed_count_delegate(const EffectBodyRequest *request,uint32_t f,
    EffectBodyFunctionHead *head,EffectBodyFunctionDimensions *dimensions) {
    return effect_body_owned_head(request,f,head,dimensions);
}
static XrXirStatus closed_count_read(const XrXirCompileContext *context,
    const EffectBodyMaterial *material,uint32_t f,bool parameters,uint32_t *output) {
    size_t attempts=rp_attempts;XrCompileResourceStats before=rp_stats(context);
    EffectBodyMaterial held=*material;EffectBodyFunction head={0};
    bool in_bounds=f<effect_body_count(material);if(in_bounds)head=material->functions[f];
    uint32_t old=output?*output:0;
    XrXirStatus status=effect_invocation_closed_count(context,material,f,parameters,output);
    CHECK(rp_attempts==attempts && rp_stats(context).allocated_bytes==before.allocated_bytes &&
        rp_stats(context).live_bytes==before.live_bytes && !memcmp(material,&held,sizeof(held)));
    if(in_bounds)CHECK(!memcmp(&material->functions[f],&head,sizeof(head)));
    if(status!=XR_XIR_OK && output)CHECK(*output==old);return status;
}
static void closed_count_scalar_controls(const XrXirCompileContext *context,EffectBodyMaterial *material) {
    EffectBodyRequest owned=effect_body_owned_request(context,material),delegated=owned;delegated.head=closed_count_delegate;
    for(uint32_t f=0;f<effect_body_count(material);++f)for(unsigned kind=0;kind<2;++kind){
        const EffectBodyFunction *head=&material->functions[f];uint32_t expected=kind?head->parameter_count:head->instruction_count;
        uint32_t actual=UINT32_MAX;uint64_t start=rp_stats(context).work;
        CHECK(closed_count_read(context,material,f,kind!=0,&actual)==XR_XIR_OK && actual==expected &&
            rp_stats(context).work-start==11+sizeof(actual));
        for(unsigned reader=0;reader<2;++reader){EffectBodyFunctionDimensions dimensions={0};start=rp_stats(context).work;
            CHECK(effect_body_request_dimensions(reader?&delegated:&owned,f,&dimensions)==XR_XIR_OK &&
                dimensions.parameter_count==head->parameter_count && dimensions.instruction_count==head->instruction_count &&
                (kind?dimensions.parameter_count:dimensions.instruction_count)==actual &&
                rp_stats(context).work-start==11+sizeof(dimensions));}
        uint64_t limit=context->resources->limits.work;
        for(uint64_t cut=0;cut<=11+sizeof(actual);++cut){start=rp_stats(context).work;context->resources->limits.work=start+cut;
            actual=UINT32_MAX;CHECK(closed_count_read(context,material,f,kind!=0,&actual)==
                (cut==11+sizeof(actual)?XR_XIR_OK:XR_XIR_BUDGET));
            if(cut<11+sizeof(actual))CHECK(closed_count_read(context,material,f,kind!=0,&actual)==XR_XIR_BUDGET && actual==UINT32_MAX);
            else CHECK(actual==expected);context->resources->limits.work=limit;
        }
        ++closed_count_values;
    }
}
static void closed_count_faults(const XrXirCompileContext *context,EffectBodyMaterial *material) {
    EffectBodyFunction *head=&material->functions[0],original=*head;
    for(unsigned fault=0;fault<7;++fault){
        if(!fault){head->name_length=1;head->name=NULL;}
        else if(fault==1){head->parameter_count=1;head->parameters=NULL;}
        else if(fault==2){head->block_count=1;head->blocks=NULL;}
        else if(fault==3){head->operand_count=1;head->operands=NULL;}
        else if(fault==4){head->instruction_count=1;head->runs=NULL;head->run_count=1;}
        else if(fault==5){head->instruction_count=1;head->run_count=0;}
        else head->run_count=head->run_capacity+1;
        for(unsigned kind=0;kind<2;++kind){uint32_t output=UINT32_MAX;
            CHECK(closed_count_read(context,material,0,kind!=0,&output)==XR_XIR_BAD_STRUCTURE);
            CHECK(closed_count_read(context,material,0,kind!=0,&output)==XR_XIR_BAD_STRUCTURE);}
        *head=original;
    }
    XirMetadataDomain original_domain=material->domain;
    for(unsigned fault=0;fault<4;++fault){
        if(!fault)material->domain.types=NULL;
        else if(fault==1)material->domain.declarations=(void *)material;
        else if(fault==2)material->domain.generics=(void *)material;
        else material->domain.defaults=(void *)material;
        uint32_t output=UINT32_MAX;
        CHECK(closed_count_read(context,material,0,false,&output)==XR_XIR_BAD_STRUCTURE);material->domain=original_domain;
    }
    uint32_t output=UINT32_MAX;CHECK(closed_count_read(context,material,effect_body_count(material),true,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(closed_count_read(context,material,0,true,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirCompileContext foreign=rp_owner(rp_caps());uint64_t baseline=rp_stats(&foreign).live_bytes,work=rp_stats(&foreign).work;
    CHECK(closed_count_read(&foreign,material,0,true,&output)==XR_XIR_BAD_STRUCTURE && rp_stats(&foreign).work==work);
    rp_owner_free(&foreign,baseline);
    CHECK(effect_invocation_closed_count(NULL,material,0,true,&output)==XR_XIR_BAD_STRUCTURE && output==UINT32_MAX);
    CHECK(effect_invocation_closed_count(context,NULL,0,true,&output)==XR_XIR_BAD_STRUCTURE && output==UINT32_MAX);
}
static XrXirStatus closed_count_closure(const XrXirCompileContext *context,const EffectInvocationFinal *final) {
    EffectInvocationFinal held=*final;EffectInvocationNode nodes[64];EffectInvocationEdge edges[128];
    CHECK(final->count<=64 && final->edge_count<=128);
    memcpy(nodes,final->nodes,(size_t)final->count*sizeof(*nodes));
    if(final->edge_count)memcpy(edges,final->edges,(size_t)final->edge_count*sizeof(*edges));
    RootParameterMark physical=rp_mark();uint64_t live=rp_stats(context).live_bytes;
    XrXirStatus status=effect_invocation_closed_read(context,final);
    CHECK(!memcmp(final,&held,sizeof(held)) && !memcmp(nodes,final->nodes,(size_t)final->count*sizeof(*nodes)) &&
        (!final->edge_count || !memcmp(edges,final->edges,(size_t)final->edge_count*sizeof(*edges))) &&
        rp_stats(context).live_bytes==live);rp_balanced(physical);return status;
}
static void closed_count_closure_controls(const XrXirCompileContext *context,EffectInvocationFinal *final) {
    CHECK(closed_count_closure(context,final)==XR_XIR_OK);
    for(uint32_t n=0;n<final->count;++n){uint32_t held=final->nodes[n].intrinsic_mask;
        final->nodes[n].intrinsic_mask^=XR_XIR_CALLABLE_ROOT_REQUIRED;
        CHECK(closed_count_closure(context,final)==XR_XIR_BAD_STRUCTURE);
        final->nodes[n].intrinsic_mask=held;CHECK(closed_count_closure(context,final)==XR_XIR_OK);
    }
    uint64_t entry=rp_stats(context).work;CHECK(closed_count_closure(context,final)==XR_XIR_OK);
    uint64_t exact=rp_stats(context).work-entry,limit=context->resources->limits.work;CHECK(exact);closed_count_work_units+=exact;
    for(uint64_t cut=0;cut<=exact;++cut){entry=rp_stats(context).work;CHECK(exact<=limit-entry);
        context->resources->limits.work=entry+cut;
        CHECK(closed_count_closure(context,final)==(cut==exact?XR_XIR_OK:XR_XIR_BUDGET));
        if(cut<exact)CHECK(closed_count_closure(context,final)==XR_XIR_BUDGET);
        context->resources->limits.work=limit;
    }
}
static XrXirStatus closed_count_run(const XrXirCompileContext *context,uint32_t mode,bool controls) {
    InvocationClassCellFixture fixture;invocation_class_cell_fixture(&fixture,mode);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;EffectInvocationWorkspace replay={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.source.module,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if(status==XR_XIR_OK){EffectInvocationCertificate *certificate=effects->invocations;CHECK(certificate && certificate->final);
        status=closed_count_closure(context,certificate->final);
        if(status==XR_XIR_OK && controls){closed_count_scalar_controls(context,certificate->body);
            closed_count_faults(context,certificate->body);if(!mode)closed_count_closure_controls(context,certificate->final);}
        if(status==XR_XIR_OK)status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&replay);
        EffectBodyRequest body=effect_body_owned_request(context,certificate->body);
        if(status==XR_XIR_OK)status=effect_invocation_class_verify_read(&replay,&body);
    }
    effect_invocation_read_close(&replay);xr_xir_compile_effects_free(effects);return status;
}
static void invocation_closed_count_controls(void) {
    RootParameterMark physical=rp_mark();XrCompileResourceStats measured={0};
    for(uint32_t mode=0;mode<7;++mode){XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(closed_count_run(&context,mode,true)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);}
    XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(closed_count_run(&context,6,false)==XR_XIR_OK);measured=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){XrCompileResourceLimits limits=rp_caps();
        if(!axis)limits.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)limits.live_bytes=measured.peak_bytes-minus;else limits.work=measured.work-minus;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(closed_count_run(&context,6,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,baseline);rp_balanced(physical);}
    CHECK(closed_count_values && closed_count_work_units);
    fprintf(stderr,"Closed count fields: all7 genuine Class/Cell certificates after Checked/producer death, %llu actual counts versus literal fields and original owned/delegated dimensions with exact fee oracle, seven complete head presence and four material domain plus bounds/null/foreign ledger refusals, each scalar work cut and same finite-owner retry, no allocation; every genuine node inflated or missing OR rejected, complete closure every work unit through%llu with repeated Budget, unchanged final/node/edge payload and physical baseline, fresh independent Class verification; whole genuine-fixture six axes%llu/%llu/%llu exact/minusone PASS\n",
        (unsigned long long)closed_count_values,(unsigned long long)closed_count_work_units,(unsigned long long)measured.allocated_bytes,
        (unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_CLOSED_COUNT_CASES_H
