/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_integer_error_run_cases.h - Owned integer Error replay and scalar comparison
 */
static XrXirStatus integer_scalar_read(const EffectBodyRequest *request,uint32_t f,
    uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output,
    EffectBodyInstructionFields *fields) {
    return effect_body_owned_instruction(request,f,i,storage,output,fields);
}
static void integer_body_replay(const XrXirCompileContext *work,const XrXirEffects *effects,bool scalar,
    XrXirStatus expected) {
    uint64_t baseline=rp_stats(work).live_bytes;
    EffectBodyRequest input=effect_body_owned_request(work,effects->invocations->body);
    if (scalar) input.instruction=integer_scalar_read;
    CHECK(local_errors_request(work,&input,NULL)==expected);
    CHECK(rp_stats(work).live_bytes==baseline);
}
static void integer_owned_case(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    XrXirInstruction operations[17]={0};
    for (uint32_t i=0;i<16;++i)
        operations[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=i};
    operations[16]=(XrXirInstruction){.op=XR_XIR_RETURN};
    fixture.functions[0].instructions=operations;
    fixture.functions[0].instruction_count=fixture.entry_block.count=17;
    fixture.functions[0].operands=NULL;fixture.functions[0].operand_count=0;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    memset(&fixture,0xCE,sizeof(fixture));memset(operations,0xCE,sizeof(operations));
    EffectBodyFunction *head=&effects->invocations->body->functions[0];
    CHECK(head->run_count==2 && head->runs[0].count==16);
    EffectBodyRun saved=head->runs[0],tail=head->runs[1];
    /* A private projection reads these changed physical words. Full Source
     * admission and permission checks remain independent obligations. */
    for (uint32_t w=2;w<EFFECT_BODY_WORDS;++w) {
        head->runs[0].mask=UINT32_C(1)<<w;head->runs[0].delta[w]=UINT32_MAX;
        integer_body_replay(&context,effects,false,XR_XIR_OK);
        integer_body_replay(&context,effects,true,XR_XIR_OK);
        head->runs[0].delta[w]=0;
        integer_body_replay(&context,effects,false,XR_XIR_BAD_STRUCTURE);
        integer_body_replay(&context,effects,true,XR_XIR_BAD_STRUCTURE);
        head->runs[0]=saved;
    }
    head->runs[0].base.op=(XrXirOp)(XR_XIR_INVALID+1);
    head->runs[0].mask|=1;head->runs[0].delta[0]=UINT32_MAX;
    integer_body_replay(&context,effects,false,XR_XIR_BAD_STRUCTURE);
    integer_body_replay(&context,effects,true,XR_XIR_BAD_STRUCTURE);head->runs[0]=saved;
    head->runs[0].mask|=2;head->runs[0].delta[1]=0;
    integer_body_replay(&context,effects,false,XR_XIR_BAD_STRUCTURE);head->runs[0]=saved;
    ++head->runs[1].first;integer_body_replay(&context,effects,false,XR_XIR_BAD_STRUCTURE);
    head->runs[1]=tail;
    integer_body_replay(&context,effects,false,XR_XIR_OK);
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(!rp_live && !rp_live_bytes && !rp_allocation_observer);
}
