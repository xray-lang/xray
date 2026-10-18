/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_capture_callable_cases.h - Actual callable environment alternatives
 */
static void complete_captured_case(uint32_t mode) {
    CHECK(mode<4);bool weak=(mode&1)!=0,nested=mode>=2;
    XrXirType callback=256;uint32_t captured[2]={weak?1u:0u,weak?2u:1u};
    XrXirTypeNode type={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    XrXirTypes types={&type,1,NULL,NULL};
    XrXirInstruction entry[3]={{.op=XR_XIR_CALL,.type=callback,.immediate=1},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirInstruction factory[5]={{.op=XR_XIR_FUNCTION_REF,.type=callback,.immediate=2},
        {.op=XR_XIR_FUNCTION_REF,.type=callback,.immediate=3,.args={0,1}},{.op=XR_XIR_RETURN,.args={1,0}}};
    if(weak) {
        factory[2]=factory[1];factory[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_WEAKEN,.type=callback};
        factory[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    }
    uint32_t returned=weak?2u:1u;
    if(nested) {
        ++returned;factory[returned]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=callback,.immediate=3,.args={1,1}};
        factory[returned+1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={returned,0}};
    }
    XrXirInstruction pure[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42},{.op=XR_XIR_RETURN}};
    XrXirInstruction apply[2]={{.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirInstruction init={.op=XR_XIR_RETURN};
    XrXirBlock blocks[5]={{.count=3},{.count=returned+2},{.count=2},{.count=2},{.count=1}};
    XrXirFunction functions[5]={
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.instructions=entry,.instruction_count=3,.blocks=&blocks[0],.block_count=1},
        {.name="factory",.name_length=7,.result=callback,.instructions=factory,.instruction_count=blocks[1].count,
            .blocks=&blocks[1],.block_count=1,.operands=captured,.operand_count=nested?2u:1u},
        {.name="pure",.name_length=4,.result=XR_XIR_I64,.instructions=pure,.instruction_count=2,.blocks=&blocks[2],.block_count=1},
        {.name="apply",.name_length=5,.result=XR_XIR_I64,.parameters=&callback,.parameter_count=1,
            .instructions=apply,.instruction_count=2,.blocks=&blocks[3],.block_count=1},
        {.name="init",.name_length=4,.instructions=&init,.instruction_count=1,.blocks=&blocks[4],.block_count=1}};
    const XrXirSourceModule source={.name="captured",.name_length=8,.initializer=4};
    const XrXirFunctionIdentity identities[5]={0};
    const XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.entry_function=0};
    const XrXirModule module={.stage=XR_XIR_BUILT,.linkage_kind=XR_XIR_PROGRAM,.types=&types,
        .functions=functions,.function_count=5,.declarations=&declarations};
    XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&work,&module,&checked,&diagnostic);
    if(status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    fprintf(stderr,"captured mode%u status%u f%u b%u i%u unknown%u work%llu\n",mode,status,
        diagnostic.function,diagnostic.block,diagnostic.instruction,effects?xr_xir_effects_error_unknown(effects,0):99u,
        (unsigned long long)pc_stats(&work).work);
    CHECK(status==XR_XIR_OK && xr_xir_effects_error_unknown(effects,0)==weak);
    xr_xir_compile_artifact_free(checked);memset(factory,0xCC,sizeof(factory));memset(functions,0xCC,sizeof(functions));
    EffectInvocationFinal *final=effects->invocations->final;
    complete_verify(&work,final,XR_XIR_OK);
    uint64_t before_work=pc_stats(&work).work;size_t before_attempts=attempts;uint64_t physical=live_bytes;
    CHECK(error_finite_outcomes_project(&work,effects)==XR_XIR_BAD_STRUCTURE);
    CHECK(pc_stats(&work).work==before_work && attempts==before_attempts && live_bytes==physical);
    pc_allocate_hook=edges_forbid;pc_free_hook=edges_forbid;
    CHECK(error_finite_outcomes_match(&work,final->material,final->outcomes)==XR_XIR_OK);
    pc_allocate_hook=NULL;pc_free_hook=NULL;CHECK(attempts==before_attempts && live_bytes==physical);
    CHECK(final->capture.count<512 && final->capture.count<final->capture.capacity);
    EffectCaptureFact saved[512];uint32_t count=final->capture.count;
    memcpy(saved,final->capture.facts,count*sizeof(*saved));
    bool empty=false,descending=false;
    for(uint32_t p=0;p<count;++p) if(saved[p].word==EFFECT_CAPTURE_FLAGS && saved[p].bit>EFFECT_CAPTURE_COMPLETE) {
        if(!saved[p].rank && !empty) {
            --final->capture.count;
            memmove(final->capture.facts+p,final->capture.facts+p+1,(size_t)(count-p-1)*sizeof(*saved));
            complete_verify(&work,final,XR_XIR_BAD_STRUCTURE);empty=true;
        } else if(saved[p].rank && !descending) {
            final->capture.facts[p].rank=0;complete_verify(&work,final,XR_XIR_BAD_STRUCTURE);descending=true;
        }
        final->capture.count=count;memcpy(final->capture.facts,saved,count*sizeof(*saved));
    }
    CHECK(empty && (weak || descending));
    edges_capture_fake(final,(EffectCaptureFact){final->roots[3],0,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_COMPLETE+1,0});
    complete_verify(&work,final,XR_XIR_BAD_STRUCTURE);
    final->capture.count=count;memcpy(final->capture.facts,saved,count*sizeof(*saved));
    complete_verify(&work,final,XR_XIR_OK);
    xr_xir_compile_effects_free(effects);pc_release(&work,baseline);
}
