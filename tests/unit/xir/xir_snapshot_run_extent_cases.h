/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_snapshot_run_extent_cases.h - Actual immutable record allocation controls
 */
#ifndef XIR_SNAPSHOT_RUN_EXTENT_CASES_H
#define XIR_SNAPSHOT_RUN_EXTENT_CASES_H
static XrXirStatus run_extent_rows(const XrXirCompileContext *context,const EffectBodyFunction *body,
    const XrXirInstruction expected[11]) {
    CHECK(body->instruction_count==11 && body->run_count==body->run_capacity && body->runs);
    const CompileAllocation *allocation=(const CompileAllocation *)body->runs-1;
    CHECK(allocation->info.resources==context->resources &&
        allocation->info.bytes==(size_t)body->run_count*sizeof(*body->runs));
    EffectBodyRead read=effect_body_compile_read(context);
    for(uint32_t i=0;i<11;++i){XrXirInstruction actual;
        XrXirStatus status=effect_body_instruction_get(&read,body,i,&actual);
        if(status!=XR_XIR_OK)return status;
        CHECK(conditional_compact_same(&actual,&expected[i]));}
    return XR_XIR_OK;
}
static XrXirStatus run_extent_pack(const XrXirCompileContext *context,XrXirEffects *effects,
    const XrXirInstruction expected[11],bool inject) {
    const EffectBodyMaterial *material=effects->invocations->body;
    for(uint32_t f=0;f<effect_body_count(material);++f){const EffectBodyFunction *body=&material->functions[f];
        CHECK(body->run_count==body->run_capacity);
        if(body->runs){const CompileAllocation *a=(const CompileAllocation *)body->runs-1;
            CHECK(a->info.resources==context->resources && a->info.bytes==(size_t)body->run_count*sizeof(*body->runs));}}
    const EffectBodyFunction *real=&material->functions[3];
    XrXirStatus status=run_extent_rows(context,real,expected);if(status!=XR_XIR_OK)return status;
    EffectBodyFunction source=*real;source.run_capacity=source.instruction_count;
    CHECK(source.run_count<source.run_capacity);
    source.runs=xir_compile_alloc(context,(size_t)source.run_capacity*sizeof(*source.runs),&status);
    if(!source.runs)return status;
    memset(source.runs,0xa5,(size_t)source.run_capacity*sizeof(*source.runs));
    status=effect_body_runs_copy(context,real,source.runs);
    if(status!=XR_XIR_OK){xr_compile_resources_free(source.runs);return status;}
    if(inject){EffectBodyFunction held=source;RootParameterMark retained=rp_mark();
        uint64_t live=rp_stats(context).live_bytes;XrCompileResources *identity=context->resources;
        rp_fail_at=rp_attempts;rp_injected=false;
        CHECK(effect_body_runs_finish(context,&source)==XR_XIR_OUT_OF_MEMORY && rp_injected &&
            !memcmp(&held,&source,sizeof(source)) && context->resources==identity);
        rp_fail_at=SIZE_MAX;rp_balanced(retained);CHECK(rp_stats(context).live_bytes==live);
    }
    uint64_t live=rp_stats(context).live_bytes;
    uint64_t spare=(uint64_t)(source.run_capacity-source.run_count)*sizeof(*source.runs);
    status=effect_body_runs_finish(context,&source);
    if(status==XR_XIR_OK){CHECK(rp_stats(context).live_bytes==live-spare);status=run_extent_rows(context,&source,expected);
        if(inject){size_t attempts=rp_attempts;EffectBodyRun *held=source.runs;rp_fail_at=attempts;rp_injected=false;
            CHECK(effect_body_runs_finish(context,&source)==XR_XIR_OK && !rp_injected &&
                rp_attempts==attempts && source.runs==held);rp_fail_at=SIZE_MAX;}}
    xr_compile_resources_free(source.runs);return status;
}
static XrXirStatus run_extent_whole(const XrXirCompileContext *context,bool inject) {
    XrXirEffects *effects=NULL;XrXirInstruction expected[11];
    XrXirStatus status=row_data_fixture(context,&effects,NULL,expected);
    if(status==XR_XIR_OK)status=run_extent_pack(context,effects,expected,inject);
    xr_xir_compile_effects_free(effects);return status;
}
static void snapshot_run_extent_controls(void) {
    XrCompileResourceStats measured={0};
    for(unsigned pass=0;pass<2;++pass){RootParameterMark mark=rp_mark();
        XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
        CHECK(run_extent_whole(&context,false)==XR_XIR_OK);XrCompileResourceStats stats=rp_stats(&context);
        if(!pass)measured=stats;else CHECK(measured.allocated_bytes==stats.allocated_bytes &&
            measured.peak_bytes==stats.peak_bytes && measured.work==stats.work);
        rp_owner_free(&context,live);rp_balanced(mark);}
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){
        RootParameterMark mark=rp_mark();XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        XrXirCompileContext context=rp_owner(caps);uint64_t live=rp_stats(&context).live_bytes;
        CHECK(run_extent_whole(&context,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,live);rp_balanced(mark);}
    RootParameterMark mark=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t live=rp_stats(&context).live_bytes;CHECK(run_extent_whole(&context,true)==XR_XIR_OK);
    rp_owner_free(&context,live);rp_balanced(mark);
    fprintf(stderr,"Run extent actual allocation headers, complete eleven-row oracle after producer death, spare live reclamation, real packing OOM same-owner retry and six axes%llu/%llu/%llu exact/minusone PASS\n",
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_SNAPSHOT_RUN_EXTENT_CASES_H
