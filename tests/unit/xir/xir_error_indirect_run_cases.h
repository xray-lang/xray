/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_indirect_run_cases.h - Current opcode census and independent scalar replay
 */
#ifndef XIR_ERROR_INDIRECT_RUN_CASES_H
#define XIR_ERROR_INDIRECT_RUN_CASES_H

static uint32_t body_run_refuse=UINT32_MAX;
static XrXirStatus body_run_delegate(const EffectBodyRequest *request,uint32_t f,uint32_t i,
    XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    if(f==3 && i==body_run_refuse)return XR_XIR_BAD_STRUCTURE;
    return effect_body_owned_instruction(request,f,i,storage,output,fields);
}
static void body_run_expect(const XrXirCompileContext *context,const EffectBodyRequest *input,XrXirStatus expected) {
    uint64_t baseline=rp_stats(context).live_bytes;XrXirStatus status=local_errors_request(context,input,NULL);
    if(status!=expected)fprintf(stderr,"body run expected%u got%u\n",expected,status);
    CHECK(status==expected && rp_stats(context).live_bytes==baseline);
}
static void error_census_owned(uint32_t runs,bool present) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    if(!present) {
        fixture.apply[0]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={1,0}};
        fixture.functions[1].operands=NULL;fixture.functions[1].operand_count=0;
    }
    XrXirInstruction varied[3]={0};XrXirBlock varied_block={.count=3};uint32_t varied_operands[2]={1,1};
    if(present && (runs==2 || runs==4)) {
        varied[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
        varied[1]=fixture.apply[0];varied[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};
        fixture.functions[1].instructions=varied;fixture.functions[1].instruction_count=3;
        fixture.functions[1].blocks=&varied_block;
    } else if(present && runs==3) {
        varied[0]=varied[1]=fixture.apply[0];varied[1].args[0]=1;
        varied[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};
        fixture.functions[1].instructions=varied;fixture.functions[1].instruction_count=3;
        fixture.functions[1].blocks=&varied_block;
        fixture.functions[1].operands=varied_operands;fixture.functions[1].operand_count=2;
    }
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
    uint32_t count=2*runs-1;ops[count-1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    fixture.functions[3].instructions=ops;fixture.functions[3].instruction_count=fixture.init_block.count=count;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus prepared=xir_fixture_check(&context,&fixture.module,&checked,&diagnostic);
    if(prepared!=XR_XIR_OK)fprintf(stderr,"FIXTURE runs=%u present=%u status=%u function=%u block=%u instruction=%u reason=%u\n",
        runs,present,prepared,diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.reason);
    CHECK(prepared==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    EffectBodyRequest raw=effect_body_raw_request(&context,&fixture.module);
    body_run_expect(&context,&raw,XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));memset(ops,0xCE,sizeof(ops));memset(varied,0xCE,sizeof(varied));memset(varied_operands,0xCE,sizeof(varied_operands));
    EffectBodyMaterial *body=effects->invocations->body;CHECK(body->functions[3].run_count==runs);
    EffectBodyRequest input=effect_body_owned_request(&context,body);
    body_run_expect(&context,&input,XR_XIR_OK);
    EffectBodyRequest delegated=input;delegated.instruction=body_run_delegate;
    body_run_expect(&context,&delegated,XR_XIR_OK);body_run_refuse=0;
    body_run_expect(&context,&delegated,XR_XIR_BAD_STRUCTURE);body_run_refuse=UINT32_MAX;
    EffectBodyFunction *function=&body->functions[3];
    for(uint32_t r=0;r<runs;++r) {
        EffectBodyRun saved=function->runs[r];
        function->runs[r].count=0;body_run_expect(&context,&input,XR_XIR_BAD_STRUCTURE);function->runs[r]=saved;
        ++function->runs[r].first;body_run_expect(&context,&input,XR_XIR_BAD_STRUCTURE);function->runs[r]=saved;
        function->runs[r].mask|=1024;body_run_expect(&context,&input,XR_XIR_BAD_STRUCTURE);function->runs[r]=saved;
        function->runs[r].base.op=XR_XIR_OP_COUNT;body_run_expect(&context,&input,XR_XIR_BAD_STRUCTURE);function->runs[r]=saved;
    }
    if(runs>1) {
        EffectBodyRun saved=function->runs[0];
        for(uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
            function->runs[0].mask|=UINT32_C(1)<<w;function->runs[0].delta[w]=0;
            body_run_expect(&context,&input,XR_XIR_BAD_STRUCTURE);function->runs[0]=saved;
        }
    }
    ++function->instruction_count;body_run_expect(&context,&input,XR_XIR_BAD_STRUCTURE);--function->instruction_count;
    EffectBodyRequest foreign=input;foreign.domain.function_count++;body_run_expect(&context,&foreign,XR_XIR_BAD_STRUCTURE);
    foreign=input;foreign.domain.types=NULL;body_run_expect(&context,&foreign,XR_XIR_BAD_STRUCTURE);
    XrXirCompileContext other=rp_owner(rp_caps());uint64_t other_baseline=rp_stats(&other).live_bytes;
    body_run_expect(&other,&input,XR_XIR_BAD_STRUCTURE);rp_owner_free(&other,other_baseline);
    body_run_expect(&context,&input,XR_XIR_OK);
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(!rp_live && !rp_live_bytes && !rp_allocation_observer);
}
static void error_indirect_run_cases(void) {
    for(uint32_t runs=1;runs<=5;++runs)for(uint32_t present=0;present<2;++present)error_census_owned(runs,present!=0);
}
#endif // XIR_ERROR_INDIRECT_RUN_CASES_H
