/* Real GO-check and native authority failures share one owned memory lifetime. */
#ifndef XIR_ROOT_GO_RESOURCES_H
#define XIR_ROOT_GO_RESOURCES_H
#include "xir_program_input_fixture.h"
static void go_check_occupied(void) {
    EffectMark mark=effect_mark();XrXirCompileContext context=effect_owner_new(effect_caps());
    XrCompileResourceStats before=effect_stats(&context);
    XrXirArtifact *output=(XrXirArtifact *)(uintptr_t)1;attempts=0;
    CHECK(go_shape_check(&context,true,true,&output,NULL)==XR_XIR_BAD_STRUCTURE&&output==(XrXirArtifact *)(uintptr_t)1);
    XrCompileResourceStats after=effect_stats(&context);
    CHECK(!attempts&&after.held_bytes==before.held_bytes&&after.allocated_bytes==before.allocated_bytes&&after.allocation_count==before.allocation_count);
    effect_owner_free(&context,before.held_bytes);effect_mark_check(mark);
}
static void go_check_failures(void) {
    EffectMark mark=effect_mark();XrXirCompileContext context=effect_owner_new(effect_caps());
    uint64_t baseline=effect_stats(&context).held_bytes;XrXirArtifact *output=NULL;
    attempts=0;CHECK(go_shape_check(&context,true,true,&output,NULL)==XR_XIR_BAD_TYPE&&!output);
    size_t sites=attempts;XrCompileResourceStats measured=effect_stats(&context);CHECK(sites);
    effect_owner_free(&context,baseline);effect_mark_check(mark);
    size_t selected=0;
    for(size_t point=0;point<sites;++point){
        if(!effect_full_faults && point && point+1!=sites)continue;
        ++selected;
        context=effect_owner_new(effect_caps());baseline=effect_stats(&context).held_bytes;
        output=NULL;attempts=0;injected=false;fail_at=point;
        CHECK(go_shape_check(&context,true,true,&output,NULL)==XR_XIR_OUT_OF_MEMORY&&!output&&injected);
        XrCompileResourceStats paid=effect_stats(&context);fail_at=SIZE_MAX;
        CHECK(go_shape_check(&context,true,true,&output,NULL)==XR_XIR_BAD_TYPE&&!output);
        XrCompileResourceStats retried=effect_stats(&context);
        CHECK(retried.allocated_bytes>paid.allocated_bytes&&retried.allocation_count>paid.allocation_count);
        effect_owner_free(&context,baseline);effect_mark_check(mark);
    }
    for(unsigned minus=0;minus<2;++minus){
        XrCompileResourceLimits limits={XR_COMPILE_MEMORY_FINITE,measured.peak_bytes-minus,true};
        context=effect_owner_new(limits);baseline=effect_stats(&context).held_bytes;output=NULL;
        CHECK(go_shape_check(&context,true,true,&output,NULL)==(minus?XR_XIR_MEMORY_LIMIT:XR_XIR_BAD_TYPE)&&!output);
        effect_owner_free(&context,baseline);effect_mark_check(mark);
    }
    go_check_occupied();
    printf("GO check %zu allocation sites, %zu selected failures, held-memory and physical0\n",sites,selected);
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
    uint64_t baseline=effect_stats(&context).held_bytes;XrXirProgramPermissions *authority=NULL;
    XrXirProgramInput *program_input=NULL;
    attempts=0;CHECK(program_fixture_decode_prepare(&context,spec,&spec->packet,&program_input,&authority)==XR_XIR_OK&&authority);
    size_t sites=attempts;CHECK(sites);xir_program_permissions_free(authority);authority=NULL;
    xir_program_input_free(program_input);program_input=NULL;
    XrCompileResourceStats measured=effect_stats(&context);effect_owner_free(&context,baseline);effect_mark_check(mark);
    size_t selected=0;
    for(size_t point=0;point<sites;++point){
        if(!effect_full_faults && point && point+1!=sites)continue;
        ++selected;
        context=effect_owner_new(effect_caps());baseline=effect_stats(&context).held_bytes;
        authority=NULL;attempts=0;injected=false;fail_at=point;
        CHECK(program_fixture_decode_prepare(&context,spec,&spec->packet,&program_input,&authority)==XR_XIR_OUT_OF_MEMORY&&!authority&&injected);
        XrCompileResourceStats paid=effect_stats(&context);fail_at=SIZE_MAX;
        CHECK(program_fixture_decode_prepare(&context,spec,&spec->packet,&program_input,&authority)==XR_XIR_OK&&authority);
        XrCompileResourceStats retried=effect_stats(&context);
        CHECK(retried.allocated_bytes>paid.allocated_bytes&&retried.allocation_count>paid.allocation_count);
        xir_program_permissions_free(authority);xir_program_input_free(program_input);program_input=NULL;effect_owner_free(&context,baseline);effect_mark_check(mark);
    }
    for(unsigned minus=0;minus<2;++minus){
        XrCompileResourceLimits limits={XR_COMPILE_MEMORY_FINITE,measured.peak_bytes-minus,true};
        context=effect_owner_new(limits);baseline=effect_stats(&context).held_bytes;authority=NULL;
        CHECK(program_fixture_decode_prepare(&context,spec,&spec->packet,&program_input,&authority)==(minus?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
        CHECK(minus?!authority:authority!=NULL);xir_program_permissions_free(authority);
        xir_program_input_free(program_input);program_input=NULL;
        effect_owner_free(&context,baseline);effect_mark_check(mark);
    }
    context=effect_owner_new(effect_caps());baseline=effect_stats(&context).held_bytes;
    authority=(XrXirProgramPermissions *)(uintptr_t)1;attempts=0;XrCompileResourceStats occupied=effect_stats(&context);
    CHECK(xir_program_input_prepare(&context,spec,NULL,&authority)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=effect_stats(&context);
    CHECK(authority==(XrXirProgramPermissions *)(uintptr_t)1&&!attempts&&occupied.allocation_count==after.allocation_count&&occupied.allocated_bytes==after.allocated_bytes);
    effect_owner_free(&context,baseline);effect_mark_check(mark);
    printf("native GO authority %zu allocation sites, %zu selected failures, held-memory and physical0\n",sites,selected);
}
#endif
