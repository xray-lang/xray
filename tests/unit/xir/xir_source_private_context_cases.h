/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_private_context_cases.h - Real refiner and receiving Input lifetime
 */
#ifndef XIR_SOURCE_PRIVATE_CONTEXT_CASES_H
#define XIR_SOURCE_PRIVATE_CONTEXT_CASES_H
static const XrXirEffects *source_context_current_effects;
static const EffectBodyMaterial *source_context_input;
static EffectBodyFunction source_context_heads[8];
static EffectBodyRun source_context_runs[8][8];
static XrXirTypeNode source_context_types[32];
static char source_context_names[8][32];
static unsigned source_context_callbacks,source_context_retirements,source_context_continuations;
static void source_context_refiner_observe(const XrXirEffects *effects,bool after) {
    if(!after){CHECK(!source_context_input);++source_context_callbacks;}
    source_context_current_effects=effects;
}
/* Record only physical bytes; all real work and callbacks are forwarded.
 * This borrow ends inside the same fresh constructor, before another refiner. */
static void source_context_work_observe(const XrXirCompileContext *context,uint64_t fee,
    const char *function,bool accepted) {
    (void)context;
    if(!strcmp(function,"effect_context_owner_private_refresh")){
        CHECK(!source_context_input && fee==2*sizeof(void *));if(!accepted)return;
        CHECK(source_context_current_effects && source_context_current_effects->contexts && source_context_current_effects->invocations);
        source_context_input=source_context_current_effects->invocations->body;
        CHECK(effect_body_material_shape(source_context_input) && effect_body_count(source_context_input)<=8 && source_context_input->terms.types.count<=32);
        memcpy(source_context_heads,source_context_input->functions,(size_t)effect_body_count(source_context_input)*sizeof(*source_context_heads));
        if(source_context_input->terms.types.count)memcpy(source_context_types,source_context_input->terms.types.nodes,
            (size_t)source_context_input->terms.types.count*sizeof(*source_context_types));
        for(uint32_t f=0;f<effect_body_count(source_context_input);++f){const EffectBodyFunction *head=&source_context_input->functions[f];
            CHECK(head->run_count<=8 && head->name_length<=32);
            if(head->run_count)memcpy(source_context_runs[f],head->runs,(size_t)head->run_count*sizeof(*head->runs));
            if(head->name_length)memcpy(source_context_names[f],head->name,head->name_length);}
        ++source_context_retirements;
    }else if(source_context_input && !strcmp(function,"effect_ordinary_function")){
        CHECK(source_context_current_effects->invocations->body==source_context_input && effect_body_material_shape(source_context_input));
        CHECK(!memcmp(source_context_heads,source_context_input->functions,(size_t)effect_body_count(source_context_input)*sizeof(*source_context_heads)));
        CHECK(!source_context_input->terms.types.count || !memcmp(source_context_types,source_context_input->terms.types.nodes,
            (size_t)source_context_input->terms.types.count*sizeof(*source_context_types)));
        for(uint32_t f=0;f<effect_body_count(source_context_input);++f){const EffectBodyFunction *head=&source_context_input->functions[f];
            CHECK(!head->run_count || !memcmp(source_context_runs[f],head->runs,(size_t)head->run_count*sizeof(*head->runs)));
            CHECK(!head->name_length || !memcmp(source_context_names[f],head->name,head->name_length));}
        source_context_input=NULL;++source_context_continuations;
    }
}
static XrXirStatus source_private_context_genuine(XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrXirCompileContext context=allocation_context(limits);XrCompilerSession *session=NULL;
    XrCompilerSessionStatus made=xr_compile_session_new(context.resources,&session);
    XrXirStatus status=made==XR_COMPILER_SESSION_OK?XR_XIR_OK:made==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char source[]="fn add(x:i64)->i64{return x+1}\nexport fn answer()->i64{return add(41)}\n";
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-private-context",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=&context,.linkage_kind=XR_XIR_LIBRARY};
    XrXirSourceText text={NULL,source,sizeof(source)-1};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(!allocation_effect_work_observer && !allocation_refinement_observer);
    source_context_callbacks=source_context_retirements=source_context_continuations=0;
    source_context_current_effects=NULL;source_context_input=NULL;
    allocation_effect_work_observer=source_context_work_observe;allocation_refinement_observer=source_context_refiner_observe;
    if(status==XR_XIR_OK)status=xr_xir_compile_source_check_text(&request,&text,&result,&diagnostic,NULL);
    allocation_effect_work_observer=NULL;allocation_refinement_observer=NULL;source_context_current_effects=NULL;
    if(status==XR_XIR_OK)CHECK(source_context_callbacks>=3 && source_context_retirements &&
        source_context_retirements==source_context_continuations && !source_context_input);
    source_context_input=NULL;xr_compile_session_free(session);memset(source,0xCE,sizeof(source));
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL;
    if(status==XR_XIR_OK){const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
        const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(result.snapshot);bool found=false;
        CHECK(view && view->complete && syntax && view->declaration_count==syntax->declaration_count);
        for(uint32_t i=0;i<view->declaration_count;++i)if(view->declarations[i].name && !strcmp(view->declarations[i].name,"answer")){
            CHECK(!found && view->declarations[i].exported && !view->declarations[i].parameter_count &&
                syntax->declarations[i].name.line==2 && syntax->declarations[i].name.column==11 &&
                syntax->declarations[i].name.end_line==2 && syntax->declarations[i].name.end_column==17);found=true;}
        CHECK(found);status=xr_xir_compile_checked_write(result.checked,&packet,NULL);}
    xr_xir_compile_source_result_free(&result);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL);
    if(packet.bytes)memset(packet.bytes,0xCE,packet.length);xr_xir_compile_checked_packet_free(&packet);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(received,NULL);xr_xir_compile_artifact_free(received);
    if(stats)CHECK(xr_compile_resources_stats(context.resources,stats)==XR_COMPILE_RESOURCE_OK);
    allocation_context_close(&context);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return status;
}
static void source_private_context_cases(void) {
    XrCompileResourceStats measured={0};CHECK(source_private_context_genuine(allocation_limits(),&measured)==XR_XIR_OK);
    unsigned callbacks=source_context_callbacks,retirements=source_context_retirements;
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){XrCompileResourceLimits caps=allocation_limits();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        CHECK(source_private_context_genuine(caps,NULL)==(minus?XR_XIR_BUDGET:XR_XIR_OK));}
    fprintf(stderr,"Source private context lifetime: real refiner callbacks%u and accepted retirements%u with complete current Input headers/types/names/runs surviving into fresh constructors, independent literal query/name ranges after caller/session death and same-ledger Checked receiving after Source/packet death; six axes%llu/%llu/%llu exact/minusone physical0 PASS\n",
        callbacks,retirements,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_SOURCE_PRIVATE_CONTEXT_CASES_H
