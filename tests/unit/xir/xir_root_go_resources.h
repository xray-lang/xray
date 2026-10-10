/* Real GO-check and native authority failures retain one cumulative ledger. */
#ifndef XIR_ROOT_GO_RESOURCES_H
#define XIR_ROOT_GO_RESOURCES_H
static void go_check_failures(void) {
    EffectMark mark=effect_mark();XrXirCompileContext context=effect_owner_new(effect_caps());
    uint64_t baseline=effect_stats(&context).live_bytes;XrXirArtifact *output=NULL;
    attempts=0;CHECK(go_shape_check(&context,true,true,&output,NULL)==XR_XIR_BAD_TYPE&&!output);
    size_t sites=attempts;XrCompileResourceStats measured=effect_stats(&context);CHECK(sites);
    effect_owner_free(&context,baseline);effect_mark_check(mark);
    for(size_t point=0;point<sites;++point){
        context=effect_owner_new(effect_caps());baseline=effect_stats(&context).live_bytes;
        output=NULL;attempts=0;injected=false;fail_at=point;
        CHECK(go_shape_check(&context,true,true,&output,NULL)==XR_XIR_OUT_OF_MEMORY&&!output&&injected);
        XrCompileResourceStats paid=effect_stats(&context);fail_at=SIZE_MAX;
        CHECK(go_shape_check(&context,true,true,&output,NULL)==XR_XIR_BAD_TYPE&&!output);
        XrCompileResourceStats retried=effect_stats(&context);
        CHECK(retried.allocated_bytes>paid.allocated_bytes&&retried.work>paid.work);
        effect_owner_free(&context,baseline);effect_mark_check(mark);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){
        XrCompileResourceLimits limits=effect_caps();
        uint64_t *limit=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        *limit=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)-minus;
        context=effect_owner_new(limits);baseline=effect_stats(&context).live_bytes;output=NULL;
        CHECK(go_shape_check(&context,true,true,&output,NULL)==(minus?XR_XIR_BUDGET:XR_XIR_BAD_TYPE)&&!output);
        effect_owner_free(&context,baseline);effect_mark_check(mark);
    }
    context=effect_owner_new(effect_caps());baseline=effect_stats(&context).live_bytes;
    output=(XrXirArtifact *)(uintptr_t)1;attempts=0;
    CHECK(go_shape_check(&context,true,true,&output,NULL)==XR_XIR_BAD_TYPE&&output==(XrXirArtifact *)(uintptr_t)1);
    CHECK(attempts&&effect_stats(&context).live_bytes==baseline);
    effect_owner_free(&context,baseline);effect_mark_check(mark);
    printf("GO check %zu real allocation failures, retry/no-refund, exact-minus1 and occupied-output physical0\n",sites);
}
static void go_authority_oracles(const XrXirModule *module,const XrXirProgramPermissions *authority) {
    const char *safe[]={"pure","worker","constRead","constAtomicRead","atomicParameter","local","localCell","localBox"};
    const char *bad[]={"mutableRead","mutableAtomicContainerRead","constBoxRead","rootA","rootB",
        "defaultUse","cleanupOwner","indirect","indirectInvoke","mixed","mixedRelay","goParent"};
    for(size_t i=0;i<sizeof(safe)/sizeof(safe[0]);++i)CHECK(authority->entries[root_find(module,safe[i])].worker==XR_XIR_OK);
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i)CHECK(authority->entries[root_find(module,bad[i])].worker==XR_XIR_BAD_TYPE);
    for(uint32_t f=0;f<module->function_count;++f){
        const XrXirFunctionIdentity *identity=&module->declarations->functions[f];
        if(identity->cleanup_owner||identity->test_role||module->declarations->modules[identity->module].initializer==f)
            CHECK(authority->entries[f].worker==XR_XIR_BAD_TYPE);
    }
    for(uint32_t s=0;s<module->declarations->slot_count;++s)
        if(module->declarations->slots[s].mutable)CHECK(authority->entries[(uint64_t)module->function_count+s].worker==XR_XIR_BAD_TYPE);
}
static void go_authority_failures(const XrXirProgramSpec *spec) {
    EffectMark mark=effect_mark();XrXirCompileContext context=effect_owner_new(effect_caps());
    uint64_t baseline=effect_stats(&context).live_bytes;XrXirProgramPermissions *authority=NULL;
    attempts=0;CHECK(xr_xir_compile_program_proof_verify(&context,spec,&spec->proof,&authority)==XR_XIR_OK&&authority);
    size_t sites=attempts;CHECK(sites);xir_program_permissions_free(authority);authority=NULL;
    XrCompileResourceStats measured=effect_stats(&context);effect_owner_free(&context,baseline);effect_mark_check(mark);
    for(size_t point=0;point<sites;++point){
        context=effect_owner_new(effect_caps());baseline=effect_stats(&context).live_bytes;
        authority=NULL;attempts=0;injected=false;fail_at=point;
        CHECK(xr_xir_compile_program_proof_verify(&context,spec,&spec->proof,&authority)==XR_XIR_OUT_OF_MEMORY&&!authority&&injected);
        XrCompileResourceStats paid=effect_stats(&context);fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_program_proof_verify(&context,spec,&spec->proof,&authority)==XR_XIR_OK&&authority);
        XrCompileResourceStats retried=effect_stats(&context);
        CHECK(retried.allocated_bytes>paid.allocated_bytes&&retried.work>paid.work);
        xir_program_permissions_free(authority);effect_owner_free(&context,baseline);effect_mark_check(mark);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){
        XrCompileResourceLimits limits=effect_caps();
        uint64_t *limit=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        *limit=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)-minus;
        context=effect_owner_new(limits);baseline=effect_stats(&context).live_bytes;authority=NULL;
        CHECK(xr_xir_compile_program_proof_verify(&context,spec,&spec->proof,&authority)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(minus?!authority:authority!=NULL);xir_program_permissions_free(authority);
        effect_owner_free(&context,baseline);effect_mark_check(mark);
    }
    context=effect_owner_new(effect_caps());baseline=effect_stats(&context).live_bytes;
    authority=(XrXirProgramPermissions *)(uintptr_t)1;attempts=0;XrCompileResourceStats occupied=effect_stats(&context);
    CHECK(xr_xir_compile_program_proof_verify(&context,spec,&spec->proof,&authority)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=effect_stats(&context);
    CHECK(authority==(XrXirProgramPermissions *)(uintptr_t)1&&!attempts&&occupied.work==after.work&&occupied.allocated_bytes==after.allocated_bytes);
    effect_owner_free(&context,baseline);effect_mark_check(mark);
    printf("native GO authority %zu real allocation failures, retry/no-refund, axes and early occupied-output physical0\n",sites);
}
#endif
