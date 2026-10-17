/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_final_row_lifetime_cases.h - Final receiving after row data death
 */
#ifndef XIR_INVOCATION_FINAL_ROW_LIFETIME_CASES_H
#define XIR_INVOCATION_FINAL_ROW_LIFETIME_CASES_H
static void *final_rows_blocks[33];
static bool final_rows_dead[33],final_rows_sample;
static size_t final_rows_count,final_rows_deaths,final_rows_allocations,final_rows_bytes;
static uint64_t final_rows_allocation_work[2];
static EffectInvocationRows *final_rows_observed;
static XrCompileResources *final_rows_resource;
static void final_rows_release_observe(void *pointer) {
    for(size_t i=0;i<final_rows_count;++i)if(pointer==final_rows_blocks[i] && !final_rows_dead[i]){
        final_rows_dead[i]=true;++final_rows_deaths;
    }
}
static void final_rows_allocate_observe(void *pointer) {
    (void)pointer;
    CHECK(final_rows_observed && final_rows_deaths==final_rows_count && !final_rows_observed->functions &&
        !final_rows_observed->material && !final_rows_observed->resources && !final_rows_observed->count);
    CHECK(final_rows_allocations<2);
    if(final_rows_sample)final_rows_allocation_work[final_rows_allocations]=final_rows_resource->stats.work;
    ++final_rows_allocations;
}
static void final_rows_track(EffectInvocationRows *rows,XrCompileResources *resources) {
    CHECK(!rp_release_observer && !rp_allocation_observer && rows->functions && rows->count<=32);
    final_rows_count=final_rows_deaths=final_rows_allocations=final_rows_bytes=0;
    memset(final_rows_dead,0,sizeof(final_rows_dead));
    final_rows_blocks[final_rows_count++]=(CompileAllocation *)rows->functions-1;
    for(uint32_t f=0;f<rows->count;++f)if(rows->functions[f].memory)
        final_rows_blocks[final_rows_count++]=(CompileAllocation *)rows->functions[f].memory-1;
    for(size_t i=0;i<final_rows_count;++i){const CompileAllocation *allocation=final_rows_blocks[i];
        CHECK(allocation->info.resources==resources);final_rows_bytes+=allocation->info.bytes+sizeof(*allocation);}
    final_rows_observed=rows;final_rows_resource=resources;
}
static void final_rows_refusals(const XrXirCompileContext *context,EffectInvocationWorkspace *owner,
    EffectInvocationFinal *target) {
    EffectInvocationRows *rows=owner->physical_rows;CHECK(rows && rows->functions);
    RootParameterMark held=rp_mark();XrCompileResourceStats before=rp_stats(context);
    EffectInvocationFinal saved=*target;const EffectBodyMaterial *material=rows->material;
    XrCompileResources *resources=rows->resources;uint32_t count=rows->count;
    for(unsigned mode=0;mode<7;++mode){
        if(!mode)rows->material=NULL;else if(mode==1)rows->resources=NULL;else if(mode==2)++rows->count;
        else if(mode==3)owner->pending=1;else if(mode==4)target->material=NULL;
        else if(mode==5)target->nodes=NULL;else ++target->capacity;
        CHECK(effect_invocation_final_vectors_receive(context,owner,target)==XR_XIR_BAD_STRUCTURE);
        rows->material=material;rows->resources=resources;rows->count=count;owner->pending=0;*target=saved;
        XrCompileResourceStats after=rp_stats(context);CHECK(!memcmp(&before,&after,sizeof(before)));rp_balanced(held);
    }
}
static XrXirStatus final_rows_whole(const XrXirCompileContext *context,size_t inject,bool observe,bool expect_budget) {
    XrXirEffects *effects=NULL;XrXirInstruction expected[13];EffectInvocationWorkspace read={0};
    EffectInvocationRows rows={0};EffectInvocationFinal source={0},packed={0},original={0};bool receiving=false;
    XrXirStatus status=row_reference_fixture(context,&effects,expected);
    if(status==XR_XIR_OK)status=effect_invocation_read_open(context,effects,effects->invocations->declared,effects->invocations->final,&read);
    if(status==XR_XIR_OK){original=read.final;status=final_vector_source(context,effects->invocations->final,&source);}
    if(status==XR_XIR_OK){read.final=source;read.physical_rows=&rows;status=effect_invocation_class_verify(&read);}
    if(status==XR_XIR_OK){CHECK(rows.functions && rows.count>3 && rows.functions[3].instruction_storage_count==2);
        packed=source;if(observe)final_rows_refusals(context,&read,&packed);
        RootParameterMark held=rp_mark();final_rows_track(&rows,context->resources);
        RootParameterMark retired={held.blocks-final_rows_count,held.bytes-final_rows_bytes};
        XrCompileResources *identity=context->resources;
        if(observe){rp_release_observer=final_rows_release_observe;rp_allocation_observer=final_rows_allocate_observe;}
        if(inject!=SIZE_MAX){rp_fail_at=rp_attempts+inject;rp_injected=false;}
        unsigned char held_output[sizeof(packed)];memcpy(held_output,&packed,sizeof(packed));
        receiving=true;
        status=effect_invocation_final_vectors_receive(context,&read,&packed);rp_fail_at=SIZE_MAX;
        rp_release_observer=NULL;rp_allocation_observer=NULL;final_rows_observed=NULL;
        if(observe && (status==XR_XIR_OK || inject!=SIZE_MAX || expect_budget))CHECK(final_rows_deaths==final_rows_count);
        if(inject!=SIZE_MAX){CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && context->resources==identity &&
                !memcmp(held_output,&packed,sizeof(packed)) && !rows.functions);rp_balanced(retired);
            status=effect_invocation_final_vectors_receive(context,&read,&packed);CHECK(status==XR_XIR_OK && context->resources==identity);}
        if(expect_budget){CHECK(status==XR_XIR_BUDGET && !memcmp(held_output,&packed,sizeof(packed)) && !rows.functions);
            rp_balanced(retired);CHECK(effect_invocation_final_vectors_receive(context,&read,&packed)==XR_XIR_BUDGET &&
                context->resources==identity && !memcmp(held_output,&packed,sizeof(packed)));rp_balanced(retired);}
        if(status==XR_XIR_OK){CHECK(!rows.functions);final_vector_same(&packed,effects->invocations->final);
            read.final=packed;status=effect_invocation_class_verify(&read);
            if(status==XR_XIR_OK){CHECK(rows.functions && rows.functions[3].instruction_storage_count==2);
                EffectBodyRead body_read=effect_body_compile_read(context);
                for(uint32_t i=0;i<13;++i){XrXirInstruction value;
                    status=effect_body_instruction_get(&body_read,&read.final.material->functions[3],i,&value);
                    if(status!=XR_XIR_OK)break;CHECK(conditional_compact_same(&value,&expected[i]));}}
        }
    }
    if(expect_budget)CHECK(receiving);
    effect_invocation_rows_drop(&rows);read.physical_rows=NULL;read.final=original;effect_invocation_read_close(&read);
    if(source.material){effect_invocation_final_vectors_release(&packed,&source);
        effect_invocation_final_vectors_release(&source,effects->invocations->final);}
    xr_xir_compile_effects_free(effects);return status;
}
static void invocation_final_row_lifetime_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    final_rows_sample=true;CHECK(final_rows_whole(&context,SIZE_MAX,true,false)==XR_XIR_OK);final_rows_sample=false;
    CHECK(final_rows_allocations==2);uint64_t cuts[2]={final_rows_allocation_work[0],final_rows_allocation_work[1]};
    rp_owner_free(&context,baseline);rp_balanced(physical);
    for(size_t at=0;at<2;++at){physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
        CHECK(final_rows_whole(&context,at,true,false)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);}
    for(size_t at=0;at<2;++at){physical=rp_mark();XrCompileResourceLimits caps=rp_caps();caps.work=cuts[at];
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
        CHECK(final_rows_whole(&context,SIZE_MAX,true,true)==XR_XIR_BUDGET);rp_owner_free(&context,baseline);rp_balanced(physical);}
    XrCompileResourceStats measured={0};
    for(unsigned pass=0;pass<2;++pass){physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
        CHECK(final_rows_whole(&context,SIZE_MAX,false,false)==XR_XIR_OK);XrCompileResourceStats stats=rp_stats(&context);
        if(!pass)measured=stats;else CHECK(!memcmp(&measured,&stats,sizeof(stats)));rp_owner_free(&context,baseline);rp_balanced(physical);}
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){physical=rp_mark();XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;
        else caps.work=measured.work-minus;context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
        CHECK(final_rows_whole(&context,SIZE_MAX,false,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,baseline);rp_balanced(physical);}
    fprintf(stderr,"Final receiving real13-row Class: every row allocation dies before first vector allocation, seven binding/target refusals unchanged, complete node/edge oracle and fresh Class/full13-row replay after data death, both actual2 OOM and allocation-admitted work cutoffs with same finite-owner retry, six axes%llu/%llu/%llu exact/minusone PASS\n",
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_FINAL_ROW_LIFETIME_CASES_H
