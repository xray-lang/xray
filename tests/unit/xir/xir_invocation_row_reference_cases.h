/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_row_reference_cases.h - Real retained and expanded row lifetimes
 */
#ifndef XIR_INVOCATION_ROW_REFERENCE_CASES_H
#define XIR_INVOCATION_ROW_REFERENCE_CASES_H
static XrXirStatus row_reference_fixture(const XrXirCompileContext *context,
    XrXirEffects **effects,XrXirInstruction expected[13]) {
    InvocationClassRowsFixture f;invocation_class_rows_fixture(&f,1);
    XrXirInstruction entry[13];memcpy(entry+2,f.entry,sizeof(f.entry));
    entry[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=36};
    entry[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=38};
    entry[7].args[0]+=2;entry[12].args[0]+=2;
    const unsigned values[]={0,1,3,5,6};
    for(unsigned i=0;i<5;++i)f.entry_operands[values[i]]+=2;
    f.entry_blocks[0].count+=2;
    for(unsigned i=1;i<4;++i)f.entry_blocks[i].first+=2;
    f.functions[3].instructions=entry;f.functions[3].instruction_count=13;
    if(expected)memcpy(expected,entry,sizeof(entry));
    XrXirArtifact *checked=NULL;XrXirStatus status=xir_fixture_check(context,&f.module,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_analyze(checked,effects);
    xr_xir_compile_artifact_free(checked);memset(entry,0xce,sizeof(entry));memset(&f,0xce,sizeof(f));
    if(status!=XR_XIR_OK){xr_xir_compile_effects_free(*effects);*effects=NULL;}
    return status;
}
static void row_reference_oracle(EffectInvocationFlow *flow,EffectInvocationRows *rows,
    const XrXirInstruction expected[13]) {
    const XrXirCompileContext *context=flow->owner->work;
    EffectInvocationRowData *data=&rows->functions[3];
    const EffectBodyFunction *body=&rows->material->functions[3];
    CHECK(data->instruction_count==13 && data->instruction_storage_count==2 &&
        data->instructions && data->instruction_refs && body->runs[0].count==3 &&
        data->instruction_refs[0]==&body->runs[0].base &&
        data->instruction_refs[1]==&data->instructions[0] && data->instruction_refs[2]==&data->instructions[1]);
    const CompileAllocation *allocation=(const CompileAllocation *)data->memory-1;
    size_t types=((size_t)data->values*sizeof(XrXirType)+7)&~(size_t)7;
    size_t native=(size_t)data->instruction_storage_count*sizeof(XrXirInstruction);
    size_t refs=(size_t)data->instruction_count*sizeof(*data->instruction_refs);
    size_t indices=((size_t)data->values+data->instruction_count)*sizeof(uint32_t);
    CHECK(allocation->info.resources==context->resources && allocation->info.bytes==types+native+refs+indices);
    for(uint32_t i=0;i<13;++i){XrXirInstruction scratch;const XrXirInstruction *actual=NULL;
        CHECK(effect_invocation_flow_instruction(flow,i,&scratch,&actual)==XR_XIR_OK &&
            actual==data->instruction_refs[i] && conditional_compact_same(actual,&expected[i]));}
    EffectInvocationFlow reuse=*flow;reuse.memory=NULL;
    xr_compile_resources_free(flow->memory);flow->memory=NULL;
    for(uint32_t i=0;i<13;++i){XrXirInstruction scratch;const XrXirInstruction *actual=NULL;
        CHECK(effect_invocation_flow_instruction(&reuse,i,&scratch,&actual)==XR_XIR_OK &&
            conditional_compact_same(actual,&expected[i]));}
    EffectInvocationFlow refused=reuse;refused.types_ready=false;refused.types=NULL;
    refused.instructions=NULL;refused.instruction_refs=NULL;refused.instruction_storage_count=0;
    uint32_t stored=data->instruction_storage_count;data->instruction_storage_count=14;
    CHECK(effect_invocation_flow_rows(&refused)==XR_XIR_BAD_STRUCTURE && !refused.types_ready &&
        !refused.types && !refused.instructions && !refused.instruction_refs);
    data->instruction_storage_count=stored;
    const XrXirInstruction **directory=data->instruction_refs;data->instruction_refs=NULL;
    CHECK(effect_invocation_flow_rows(&refused)==XR_XIR_BAD_STRUCTURE && !refused.types_ready && !refused.instruction_refs);
    data->instruction_refs=directory;
    XrXirInstruction *native_rows=data->instructions;data->instructions=NULL;
    CHECK(effect_invocation_flow_rows(&refused)==XR_XIR_BAD_STRUCTURE && !refused.types_ready && !refused.instructions);
    data->instructions=native_rows;
    size_t attempts=rp_attempts;rp_fail_at=attempts;rp_injected=false;
    CHECK(effect_invocation_flow_rows(&refused)==XR_XIR_OK && !rp_injected && rp_attempts==attempts &&
        refused.instructions==native_rows && refused.instruction_refs==directory);
    rp_fail_at=SIZE_MAX;
    fprintf(stderr,"Row references actual13-row Class source: retained native base and literal pairs, two independent expanded values, producer/equation scratch death, exact allocation bytes, malformed extents/directories and armed reuse PASS\n");
}
static XrXirStatus row_reference_operation(const XrXirCompileContext *context,
    XrXirEffects *effects,const XrXirInstruction expected[13]) {
    EffectInvocationCertificate *certificate=effects->invocations;EffectInvocationWorkspace read={0};
    XrXirStatus status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&read);
    EffectInvocationRows rows={0};read.physical_rows=&rows;
    EffectInvocationFlow flow={.owner=&read,.node=certificate->final->roots[3],.instruction=UINT32_MAX,.precision=true};
    EffectBodyRequest request=effect_body_owned_request(context,certificate->body);
    uint64_t limit=0;if(status==XR_XIR_OK)status=effect_invocation_class_rank_limit_count(&read,request.domain.function_count,&limit);
    if(status==XR_XIR_OK)status=effect_invocation_origins_read(&flow,&request);
    if(status==XR_XIR_OK)status=effect_invocation_class_body_match(&flow,limit);
    if(status==XR_XIR_OK)status=effect_invocation_class_terminals_match(&flow);
    if(status==XR_XIR_OK && expected)row_reference_oracle(&flow,&rows,expected);
    xr_compile_resources_free(flow.memory);read.physical_rows=NULL;effect_invocation_rows_drop(&rows);
    effect_invocation_read_close(&read);return status;
}
static XrXirStatus row_reference_whole(const XrXirCompileContext *context) {
    XrXirEffects *effects=NULL;XrXirStatus status=row_reference_fixture(context,&effects,NULL);
    if(status==XR_XIR_OK)status=row_reference_operation(context,effects,NULL);
    xr_xir_compile_effects_free(effects);return status;
}
static void row_reference_axes(void) {
    XrCompileResourceStats measured={0};
    for(unsigned pass=0;pass<2;++pass){RootParameterMark mark=rp_mark();
        XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
        CHECK(row_reference_whole(&context)==XR_XIR_OK);XrCompileResourceStats stats=rp_stats(&context);
        if(!pass)measured=stats;else CHECK(stats.allocated_bytes==measured.allocated_bytes &&
            stats.peak_bytes==measured.peak_bytes && stats.work==measured.work);
        rp_owner_free(&context,live);rp_balanced(mark);}
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){
        RootParameterMark mark=rp_mark();XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        XrXirCompileContext context=rp_owner(caps);uint64_t live=rp_stats(&context).live_bytes;
        CHECK(row_reference_whole(&context)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,live);rp_balanced(mark);}
    fprintf(stderr,"Row reference whole-fixture six axes allocated%llu/peak%llu/work%llu exact/minusone PASS\n",
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
static void invocation_row_reference_controls(void) {
    RootParameterMark mark=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t live=rp_stats(&context).live_bytes;XrXirEffects *effects=NULL;XrXirInstruction expected[13];
    CHECK(row_reference_fixture(&context,&effects,expected)==XR_XIR_OK);
    CHECK(row_reference_operation(&context,effects,expected)==XR_XIR_OK);
    RootParameterMark held=rp_mark();uint64_t held_live=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;
    CHECK(row_reference_operation(&context,effects,NULL)==XR_XIR_OK);size_t sites=rp_attempts-attempts;CHECK(sites);
    rp_balanced(held);CHECK(rp_stats(&context).live_bytes==held_live);
    for(size_t at=0;at<sites;++at){attempts=rp_attempts;rp_fail_at=attempts+at;rp_injected=false;
        XrCompileResources *identity=context.resources;
        XrXirStatus status=row_reference_operation(&context,effects,NULL);rp_fail_at=SIZE_MAX;
        CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY && context.resources==identity);
        rp_balanced(held);CHECK(rp_stats(&context).live_bytes==held_live);
        CHECK(row_reference_operation(&context,effects,NULL)==XR_XIR_OK && context.resources==identity);
        rp_balanced(held);CHECK(rp_stats(&context).live_bytes==held_live);}
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,live);rp_balanced(mark);
    fprintf(stderr,"Row reference actual scoped allocation sites%zu: every ordinal OOM and same finite ledger retry, all physical owners balanced PASS\n",sites);
    row_reference_axes();
}
#endif // XIR_INVOCATION_ROW_REFERENCE_CASES_H
