/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_error_context_replay.c - Actual Error frontiers and independent full rows
 */
#define main error_context_retained_original_main
#include "test_xir_error_cfg_pending.c"
#undef main

/* Both real body readers retain complete physical-prefix and owner checks.
 * No invocation Error frontier or global summary exists after this cutover. */
static XrXirStatus context_refused_status;
static XrXirStatus context_refused(const EffectBodyRequest *request,uint32_t f,uint32_t i,
    XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    (void)request;(void)f;(void)i;(void)storage;(void)output;(void)fields;return context_refused_status;
}
static XrXirStatus context_delegate(const EffectBodyRequest *request,uint32_t f,uint32_t i,
    XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    return effect_body_owned_instruction(request,f,i,storage,output,fields);
}
static void context_expect(const XrXirCompileContext *context,const EffectBodyRequest *input,XrXirStatus expected) {
    uint64_t live_before=pc_stats(context).live_bytes;
    CHECK(local_errors_request(context,input,NULL)==expected);
    EffectBodyRequest delegated=*input;delegated.instruction=context_delegate;
    CHECK(local_errors_request(context,&delegated,NULL)==expected);
    CHECK(pc_stats(context).live_bytes==live_before);
}
static void context_attacks(const XrXirCompileContext *context,EffectBodyRequest *input,uint32_t runs) {
    EffectBodyFunction *function=&((EffectBodyMaterial *)input->owner)->functions[0];CHECK(function->run_count==runs);
    context_expect(context,input,XR_XIR_OK);
    for(uint32_t r=0;r<runs;++r) {
        EffectBodyRun *run=&function->runs[r],saved=*run;
        run->count=0;context_expect(context,input,XR_XIR_BAD_STRUCTURE);*run=saved;
        ++run->first;context_expect(context,input,XR_XIR_BAD_STRUCTURE);*run=saved;
        for(uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
            run->mask|=UINT32_C(1)<<w;run->delta[w]=0;
            context_expect(context,input,XR_XIR_BAD_STRUCTURE);*run=saved;
        }
    }
    ++function->instruction_count;context_expect(context,input,XR_XIR_BAD_STRUCTURE);--function->instruction_count;
    const XrXirStatus statuses[]={XR_XIR_BUDGET,XR_XIR_OUT_OF_MEMORY,XR_XIR_BAD_TYPE};
    EffectBodyRequest original=*input;input->instruction=context_refused;
    for(uint32_t i=0;i<3;++i) {
        context_refused_status=statuses[i];uint64_t live_before=pc_stats(context).live_bytes;
        CHECK(local_errors_request(context,input,NULL)==statuses[i]);CHECK(pc_stats(context).live_bytes==live_before);
    }
    *input=original;context_expect(context,input,XR_XIR_OK);
}
static void context_case(uint32_t mode,uint32_t runs) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    InvocationErrorFixture fixture;ei_fixture(&fixture,mode);
    XrXirInstruction ops[9]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2},
        {.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1},
        {.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL},
        {.op=XR_XIR_CONST_FLOAT,.type=XR_XIR_F64},
        {.op=XR_XIR_CONST_FLOAT,.type=XR_XIR_F64,.immediate=1},
        {.op=XR_XIR_CONST_RUNE,.type=XR_XIR_RUNE,.immediate=97},
        {.op=XR_XIR_CONST_RUNE,.type=XR_XIR_RUNE,.immediate=98},
        {.op=XR_XIR_RETURN}};
    uint32_t count=runs*2-1;ops[count-1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    XrXirBlock block={.count=count};fixture.edge.functions[0].instructions=ops;
    fixture.edge.functions[0].instruction_count=count;fixture.edge.functions[0].blocks=&block;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.edge.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    memset(&fixture,0xa5,sizeof(fixture));memset(ops,0xa5,sizeof(ops));ei_golden(effects,mode);
    EffectBodyRequest input=effect_body_owned_request(&context,effects->invocations->body);
    context_attacks(&context,&input,runs);ei_golden(effects,mode);
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);CHECK(!live && !live_bytes);
}

int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--resources")));
    if(argc==2) {
        for(uint32_t mode=0;mode<EI_CASES;++mode) {ei_faults(mode);ei_limits(mode);}
        puts("Owned body original six allocation/refusal families and physical0 PASS");return 0;
    }
    for(uint32_t mode=0;mode<EI_CASES;++mode) for(uint32_t runs=1;runs<=5;++runs) context_case(mode,runs);
    puts("Owned body:30 declared typed/caught/opaque graphs, all10 physical columns/EOF/status failures and physical0 PASS");
    return 0;
}
