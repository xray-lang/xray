/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_fallback_run_cases.h - Ordered fallback edges and independent scalar replay
 */
#ifndef XIR_ERROR_FALLBACK_RUN_CASES_H
#define XIR_ERROR_FALLBACK_RUN_CASES_H

static uint32_t fallback_refuse=UINT32_MAX;
static XrXirStatus fallback_delegate(const EffectBodyRequest *request,uint32_t f,uint32_t i,
    XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    if(f==3 && i==fallback_refuse)return XR_XIR_BAD_STRUCTURE;
    return effect_body_owned_instruction(request,f,i,storage,output,fields);
}
static void fallback_owned(uint32_t runs,uint32_t mode) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    XrXirInstruction constants[9]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1},{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2},
        {.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1},{.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL},
        {.op=XR_XIR_CONST_FLOAT,.type=XR_XIR_F64},{.op=XR_XIR_CONST_FLOAT,.type=XR_XIR_F64,.immediate=1},
        {.op=XR_XIR_CONST_RUNE,.type=XR_XIR_RUNE,.immediate=97},{.op=XR_XIR_CONST_RUNE,.type=XR_XIR_RUNE,.immediate=98},
        {.op=XR_XIR_RETURN}};
    uint32_t count=2*runs-1;constants[count-1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    fixture.functions[3].instructions=constants;fixture.functions[3].instruction_count=fixture.init_block.count=count;
    XrXirInstruction entry[6]={0},helper[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},{.op=XR_XIR_RETURN}};
    XrXirInstruction owner={.op=XR_XIR_RETURN};XrXirBlock entry_block={.count=6},two={.count=2},one={.count=1};
    uint32_t actuals[4]={2,1,2,1};XrXirType parameter=XR_XIR_I64;
    XrXirFunction functions[6]={0};XrXirFunctionIdentity identities[6]={0};
    XrXirDefaultBinding binding={XR_XIR_DEFAULT_PARAMETER,5,0,4};XrXirDefaultTable table={&binding,1};
    if(mode) {
        memcpy(entry,fixture.entry,3*sizeof(*entry));entry[3]=entry[4]=fixture.entry[3];
        if(mode==1)entry[4].args[0]=2;
        else entry[3]=(XrXirInstruction){.op=XR_XIR_CALL_DEFAULT,.type=XR_XIR_I64,.targets={5,0}};
        entry[5]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={4,0}};
        fixture.functions[0].instructions=entry;fixture.functions[0].instruction_count=6;fixture.functions[0].blocks=&entry_block;
        fixture.functions[0].operands=actuals;fixture.functions[0].operand_count=mode==1?4:2;
    }
    if(mode==2) {
        memcpy(functions,fixture.functions,sizeof(fixture.functions));
        functions[4]=(XrXirFunction){.name="default",.name_length=7,.error_type=XR_XIR_NEVER,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=helper,.instruction_count=2};
        functions[5]=(XrXirFunction){.name="owner",.name_length=5,.error_type=XR_XIR_NEVER,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,.blocks=&one,.block_count=1,.instructions=&owner,.instruction_count=1};
        fixture.module.functions=functions;fixture.module.function_count=6;fixture.declarations.functions=identities;fixture.module.defaults=&table;
    }
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&fixture.module,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"fallback fixture runs%u mode%u status%u f%u i%u reason%u\n",runs,mode,status,diagnostic.function,diagnostic.instruction,diagnostic.reason);
    CHECK(status==XR_XIR_OK && xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    EffectBodyRequest raw=effect_body_raw_request(&context,&fixture.module);
    CHECK(local_errors_request(&context,&raw,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));memset(constants,0xCE,sizeof(constants));memset(entry,0xCE,sizeof(entry));
    memset(helper,0xCE,sizeof(helper));memset(&owner,0xCE,sizeof(owner));memset(actuals,0xCE,sizeof(actuals));memset(functions,0xCE,sizeof(functions));
    memset(identities,0xCE,sizeof(identities));memset(&binding,0xCE,sizeof(binding));memset(&table,0xCE,sizeof(table));
    EffectBodyRequest input=effect_body_owned_request(&context,effects->invocations->body);
    CHECK(effects->invocations->body->functions[3].run_count==runs);
    CHECK(local_errors_request(&context,&input,NULL)==XR_XIR_OK);
    EffectBodyRequest delegated=input;delegated.instruction=fallback_delegate;
    CHECK(local_errors_request(&context,&delegated,NULL)==XR_XIR_OK);fallback_refuse=0;
    CHECK(local_errors_request(&context,&delegated,NULL)==XR_XIR_BAD_STRUCTURE);fallback_refuse=UINT32_MAX;
    EffectBodyFunction *function=&effects->invocations->body->functions[3];
    for(uint32_t r=0;r<runs;++r) {
        EffectBodyRun saved=function->runs[r];
        function->runs[r].count=0;CHECK(local_errors_request(&context,&input,NULL)==XR_XIR_BAD_STRUCTURE);function->runs[r]=saved;
        ++function->runs[r].first;CHECK(local_errors_request(&context,&input,NULL)==XR_XIR_BAD_STRUCTURE);function->runs[r]=saved;
        function->runs[r].mask|=1024;CHECK(local_errors_request(&context,&input,NULL)==XR_XIR_BAD_STRUCTURE);function->runs[r]=saved;
        function->runs[r].base.op=XR_XIR_OP_COUNT;CHECK(local_errors_request(&context,&input,NULL)==XR_XIR_BAD_STRUCTURE);function->runs[r]=saved;
    }
    if(runs>1) {
        EffectBodyRun saved=function->runs[0];
        for(uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
            function->runs[0].mask|=UINT32_C(1)<<w;function->runs[0].delta[w]=0;
            CHECK(local_errors_request(&context,&input,NULL)==XR_XIR_BAD_STRUCTURE);function->runs[0]=saved;
        }
    }
    ++function->instruction_count;CHECK(local_errors_request(&context,&input,NULL)==XR_XIR_BAD_STRUCTURE);--function->instruction_count;
    EffectBodyRequest bad=input;bad.domain.function_count++;
    CHECK(local_errors_request(&context,&bad,NULL)==XR_XIR_BAD_STRUCTURE);
    bad=input;bad.domain.types=NULL;CHECK(local_errors_request(&context,&bad,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(local_errors_request(&context,&input,NULL)==XR_XIR_OK);
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);CHECK(!rp_live && !rp_live_bytes);
}
static void error_fallback_run_cases(void) {
    for(uint32_t runs=1;runs<=5;++runs)for(uint32_t mode=0;mode<3;++mode)fallback_owned(runs,mode);
}
#endif // XIR_ERROR_FALLBACK_RUN_CASES_H
