/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_row_phase_cases.h - Real Cell receiving after solver data death
 */
#ifndef XIR_INVOCATION_ROW_PHASE_CASES_H
#define XIR_INVOCATION_ROW_PHASE_CASES_H
static void *row_phase_native,*row_phase_directory;
static bool row_phase_native_dead,row_phase_directory_dead;
static size_t row_phase_allocations;
static void row_phase_free_observe(void *pointer) {
    if(pointer==row_phase_native)row_phase_native_dead=true;
    if(pointer==row_phase_directory)row_phase_directory_dead=true;
}
static void row_phase_alloc_observe(void *pointer) {
    (void)pointer;
    if(!row_phase_allocations++)CHECK(row_phase_native_dead && row_phase_directory_dead);
}
static XrXirStatus row_phase_operation(const XrXirCompileContext *context,
    XrXirEffects *effects,bool observe) {
    EffectInvocationCertificate *certificate=effects->invocations;EffectInvocationWorkspace read={0};
    XrXirStatus status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&read);
    EffectInvocationRows rows={0};read.physical_rows=&rows;
    EffectInvocationFlow flow={.owner=&read,.node=certificate->final->roots[3],.instruction=UINT32_MAX,.precision=true};
    EffectBodyRequest request=effect_body_owned_request(context,certificate->body);
    if(status==XR_XIR_OK)status=effect_invocation_origins_read(&flow,&request);
    xr_compile_resources_free(flow.memory);flow.memory=NULL;
    if(status==XR_XIR_OK && observe){CHECK(rows.functions && rows.functions[3].memory &&
        rows.functions[3].instruction_storage_count==2 && !rp_release_observer && !rp_allocation_observer);
        RootParameterMark held=rp_mark();XrCompileResourceStats before=rp_stats(context);
        const EffectBodyMaterial *material=rows.material;XrCompileResources *resources=rows.resources;
        uint32_t count=rows.count;
        for(unsigned mode=0;mode<4;++mode){
            if(!mode)rows.material=NULL;else if(mode==1)rows.resources=NULL;
            else if(mode==2)++rows.count;else read.pending=1;
            CHECK(effect_invocation_class_verify_released_rows(&read)==XR_XIR_BAD_STRUCTURE);
            rows.material=material;rows.resources=resources;rows.count=count;read.pending=0;
            XrCompileResourceStats after=rp_stats(context);CHECK(!memcmp(&before,&after,sizeof(before)));rp_balanced(held);}
        row_phase_native=(CompileAllocation *)rows.functions[3].memory-1;
        row_phase_directory=(CompileAllocation *)rows.functions-1;
        row_phase_native_dead=row_phase_directory_dead=false;row_phase_allocations=0;
        rp_release_observer=row_phase_free_observe;rp_allocation_observer=row_phase_alloc_observe;}
    if(status==XR_XIR_OK)status=effect_invocation_class_verify_released_rows(&read);
    if(observe){rp_release_observer=NULL;rp_allocation_observer=NULL;
        if(status==XR_XIR_OK)CHECK(row_phase_native_dead && row_phase_directory_dead && row_phase_allocations);}
    read.physical_rows=NULL;effect_invocation_rows_drop(&rows);effect_invocation_read_close(&read);return status;
}
static XrXirStatus row_phase_whole(const XrXirCompileContext *context) {
    XrXirEffects *effects=NULL;XrXirStatus status=row_reference_fixture(context,&effects,NULL);
    if(status==XR_XIR_OK)status=row_phase_operation(context,effects,false);
    xr_xir_compile_effects_free(effects);return status;
}
static void row_phase_axes(void) {
    XrCompileResourceStats measured={0};
    for(unsigned pass=0;pass<2;++pass){RootParameterMark mark=rp_mark();
        XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
        CHECK(row_phase_whole(&context)==XR_XIR_OK);XrCompileResourceStats stats=rp_stats(&context);
        if(!pass)measured=stats;else CHECK(stats.allocated_bytes==measured.allocated_bytes &&
            stats.peak_bytes==measured.peak_bytes && stats.work==measured.work);
        rp_owner_free(&context,live);rp_balanced(mark);}
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){
        RootParameterMark mark=rp_mark();XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        XrXirCompileContext context=rp_owner(caps);uint64_t live=rp_stats(&context).live_bytes;
        CHECK(row_phase_whole(&context)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,live);rp_balanced(mark);}
    fprintf(stderr,"Row phase whole-fixture six axes allocated%llu/peak%llu/work%llu exact/minusone PASS\n",
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
static void invocation_row_phase_controls(void) {
    size_t sites=0;
    for(size_t trial=0;trial<=sites;++trial){RootParameterMark mark=rp_mark();
        XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
        XrXirEffects *effects=NULL;CHECK(row_reference_fixture(&context,&effects,NULL)==XR_XIR_OK);
        RootParameterMark held=rp_mark();uint64_t held_live=rp_stats(&context).live_bytes;
        size_t attempts=rp_attempts;XrCompileResources *identity=context.resources;
        if(!trial){CHECK(row_phase_operation(&context,effects,true)==XR_XIR_OK);sites=rp_attempts-attempts;CHECK(sites);}
        else{rp_fail_at=attempts+trial-1;rp_injected=false;
            XrXirStatus status=row_phase_operation(&context,effects,false);rp_fail_at=SIZE_MAX;
            CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY && context.resources==identity);
            rp_balanced(held);CHECK(rp_stats(&context).live_bytes==held_live);
            CHECK(row_phase_operation(&context,effects,false)==XR_XIR_OK && context.resources==identity);}
        rp_balanced(held);CHECK(rp_stats(&context).live_bytes==held_live);
        xr_xir_compile_effects_free(effects);rp_owner_free(&context,live);rp_balanced(mark);}
    fprintf(stderr,"Row phase real13-row Class/Cell receiving: old native data/directory die before first new allocation, complete independent replay, every%zu OOM same finite-owner retry and physical baseline PASS\n",sites);
    row_phase_axes();
}
#endif // XIR_INVOCATION_ROW_PHASE_CASES_H
