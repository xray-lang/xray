/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_row_data_cases.h - Real scoped physical row ownership controls
 */
#ifndef XIR_INVOCATION_ROW_DATA_CASES_H
#define XIR_INVOCATION_ROW_DATA_CASES_H
static unsigned row_data_delegated_calls;
static XrXirStatus row_data_refuse_type(const EffectBodyRequest *request,uint32_t f,
    uint32_t value,XrXirType *output) {
    (void)request;(void)f;(void)value;(void)output;++row_data_delegated_calls;return XR_XIR_IO;
}
static XrXirStatus row_data_fixture(const XrXirCompileContext *context,XrXirEffects **effects,
    EffectBodyMaterial **other,XrXirInstruction expected[11]) {
    InvocationClassRowsFixture fixture;invocation_class_rows_fixture(&fixture,1);
    if(expected)memcpy(expected,fixture.entry,sizeof(fixture.entry));
    XrXirArtifact *checked=NULL;XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_analyze(checked,effects);
    if(status==XR_XIR_OK && other)status=effect_body_material_create(context,xr_xir_compile_artifact_module(checked),other);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xce,sizeof(fixture));
    if(status!=XR_XIR_OK){xr_xir_compile_effects_free(*effects);*effects=NULL;}
    return status;
}
static void row_data_reuse_controls(EffectInvocationFlow *flow,EffectInvocationRows *rows,
    const EffectBodyMaterial *other,const XrXirInstruction expected[11]) {
    const XrXirCompileContext *context=flow->owner->work;
    uint32_t body=flow->owner->final.nodes[flow->node].body;
    CHECK(body==3 && body<rows->count && flow->function->instruction_count==11);
    EffectInvocationRowData *data=&rows->functions[body];
    CHECK(data->memory && data->instructions==flow->instructions && data->types==flow->types &&
        data->instruction_refs==flow->instruction_refs && !data->instructions && !data->instruction_storage_count);
    for(unsigned i=0;i<11;++i){XrXirInstruction scratch;const XrXirInstruction *actual=NULL;
        CHECK(effect_invocation_flow_instruction(flow,i,&scratch,&actual)==XR_XIR_OK &&
            actual==data->instruction_refs[i] && conditional_compact_same(actual,&expected[i]));
        const EffectBodyRun *run=NULL;
        CHECK(effect_body_record_ref(&flow->body.read,&rows->material->functions[body],i,&run)==XR_XIR_OK);
        CHECK(actual==((run->mask&EFFECT_BODY_LITERAL_ROWS) && i!=run->first?&run->literal_tail[i-run->first-1]:&run->base));}
    EffectInvocationFlow reuse=*flow;reuse.types_ready=false;reuse.types=NULL;reuse.instructions=NULL;
    reuse.instruction_refs=NULL;reuse.instruction_storage_count=0;
    reuse.write_heads=NULL;reuse.write_next=NULL;reuse.memory=NULL;
    RootParameterMark mark=rp_mark();XrCompileResourceStats before=rp_stats(context);size_t attempts=rp_attempts;
    rp_fail_at=attempts;rp_injected=false;
    CHECK(effect_invocation_flow_rows(&reuse)==XR_XIR_OK && !rp_injected && rp_attempts==attempts &&
        reuse.types==data->types && reuse.instructions==data->instructions && reuse.instruction_refs==data->instruction_refs &&
        reuse.instruction_storage_count==data->instruction_storage_count && reuse.write_heads==data->write_heads &&
        reuse.write_next==data->write_next && reuse.types_ready);
    rp_fail_at=SIZE_MAX;XrCompileResourceStats after=rp_stats(context);
    CHECK(after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes);rp_balanced(mark);
    /* Discard the first flow's own equation scratch. The current request owns
     * the physical data independently and still serves real field reads. */
    xr_compile_resources_free(flow->memory);flow->memory=NULL;
    XrXirType type=XR_XIR_UNIT;CHECK(effect_invocation_flow_type(&reuse,0,&type)==XR_XIR_OK && type==XR_XIR_I64);
    XrXirInstruction storage={0};const XrXirInstruction *row=NULL;
    CHECK(effect_invocation_flow_instruction(&reuse,0,&storage,&row)==XR_XIR_OK && row==data->instruction_refs[0] &&
        conditional_compact_same(row,&expected[0]));
    EffectInvocationFlow refused=reuse;refused.types_ready=false;refused.types=NULL;refused.instructions=NULL;
    refused.instruction_refs=NULL;refused.instruction_storage_count=0;
    refused.write_heads=NULL;refused.write_next=NULL;
    uint32_t values=data->values;++data->values;
    CHECK(effect_invocation_flow_rows(&refused)==XR_XIR_BAD_STRUCTURE && !refused.types_ready &&
        !refused.types && !refused.instructions && !refused.instruction_refs);
    data->values=values;CHECK(effect_invocation_flow_rows(&refused)==XR_XIR_OK);
    for(unsigned missing=0;missing<8;++missing) {
        refused=reuse;refused.types_ready=false;
        if(!missing)refused.body.owner=NULL;
        else if(missing==1)refused.body.head=NULL;
        else if(missing==2)refused.body.instruction=NULL;
        else if(missing==3)refused.body.type=NULL;
        else if(missing==4)refused.body.shape=NULL;
        else if(missing==5)refused.body.domain.types=NULL;
        else if(missing==6)refused.body.read.owner=NULL;
        else refused.body.read.consume=NULL;
        attempts=rp_attempts;CHECK(effect_invocation_flow_rows(&refused)==XR_XIR_BAD_STRUCTURE &&
            !refused.types_ready && rp_attempts==attempts);
    }
    refused=reuse;refused.types_ready=false;refused.types=NULL;refused.instructions=NULL;
    refused.instruction_refs=NULL;refused.instruction_storage_count=0;
    refused.write_heads=NULL;refused.write_next=NULL;refused.body.type=row_data_refuse_type;row_data_delegated_calls=0;
    CHECK(effect_invocation_flow_rows(&refused)==XR_XIR_OK && !refused.types_ready);
    type=XR_XIR_BOOL;CHECK(effect_invocation_flow_type(&refused,0,&type)==XR_XIR_IO &&
        row_data_delegated_calls==1 && type==XR_XIR_BOOL);
    EffectInvocationWorkspace foreign_owner=*flow->owner;
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats foreign_before=rp_stats(&foreign);
    foreign_owner.work=&foreign;refused=reuse;refused.owner=&foreign_owner;refused.types_ready=false;
    refused.body=effect_body_owned_request(&foreign,flow->owner->final.material);
    CHECK(effect_invocation_flow_rows(&refused)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats foreign_after=rp_stats(&foreign);CHECK(!memcmp(&foreign_before,&foreign_after,sizeof(foreign_before)));
    rp_owner_free(&foreign,foreign_before.live_bytes);
    foreign_owner=*flow->owner;foreign_owner.final.material=(EffectBodyMaterial *)other;
    refused=reuse;refused.owner=&foreign_owner;refused.types_ready=false;refused.body=effect_body_owned_request(context,other);
    attempts=rp_attempts;CHECK(effect_invocation_flow_rows(&refused)==XR_XIR_BAD_STRUCTURE && rp_attempts==attempts);
    fprintf(stderr,"Physical row scope: literal11 full-word oracle, producer/equation scratch death, armed allocation-free reuse, count/foreign owner/foreign ledger and delegated refusal PASS\n");
}
static XrXirStatus row_data_operation(const XrXirCompileContext *context,XrXirEffects *effects,
    const EffectBodyMaterial *other,const XrXirInstruction expected[11]) {
    EffectInvocationCertificate *certificate=effects->invocations;EffectInvocationWorkspace read={0};
    XrXirStatus status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&read);
    EffectInvocationRows rows={0};read.physical_rows=&rows;
    EffectInvocationFlow flow={.owner=&read,.node=certificate->final->roots[3],.instruction=UINT32_MAX,.precision=true};
    EffectBodyRequest request=effect_body_owned_request(context,certificate->body);
    uint64_t limit=0;if(status==XR_XIR_OK)status=effect_invocation_class_rank_limit_count(&read,request.domain.function_count,&limit);
    if(status==XR_XIR_OK)status=effect_invocation_origins_read(&flow,&request);
    if(status==XR_XIR_OK)status=effect_invocation_class_body_match(&flow,limit);
    if(status==XR_XIR_OK)status=effect_invocation_class_terminals_match(&flow);
    if(status==XR_XIR_OK && expected)row_data_reuse_controls(&flow,&rows,other,expected);
    xr_compile_resources_free(flow.memory);read.physical_rows=NULL;effect_invocation_rows_drop(&rows);
    effect_invocation_read_close(&read);return status;
}
static XrXirStatus row_data_whole(const XrXirCompileContext *context) {
    XrXirEffects *effects=NULL;XrXirStatus status=row_data_fixture(context,&effects,NULL,NULL);
    if(status==XR_XIR_OK)status=row_data_operation(context,effects,NULL,NULL);
    xr_xir_compile_effects_free(effects);return status;
}
static void row_data_axes(void) {
    XrCompileResourceStats measured={0};
    for(unsigned pass=0;pass<2;++pass) {
        RootParameterMark mark=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
        CHECK(row_data_whole(&context)==XR_XIR_OK);XrCompileResourceStats stats=rp_stats(&context);
        if(!pass)measured=stats;else CHECK(stats.allocated_bytes==measured.allocated_bytes &&
            stats.peak_bytes==measured.peak_bytes && stats.work==measured.work);
        rp_owner_free(&context,live);rp_balanced(mark);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
        RootParameterMark mark=rp_mark();XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;
        else caps.work=measured.work-minus;
        XrXirCompileContext context=rp_owner(caps);uint64_t live=rp_stats(&context).live_bytes;
        CHECK(row_data_whole(&context)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,live);rp_balanced(mark);
    }
    fprintf(stderr,"Physical row whole-fixture six axes allocated%llu/peak%llu/work%llu exact/minusone PASS\n",
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
static void invocation_row_data_controls(void) {
    RootParameterMark mark=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t live=rp_stats(&context).live_bytes;
    XrXirEffects *effects=NULL;EffectBodyMaterial *other=NULL;XrXirInstruction expected[11];
    CHECK(row_data_fixture(&context,&effects,&other,expected)==XR_XIR_OK);
    CHECK(row_data_operation(&context,effects,other,expected)==XR_XIR_OK);
    RootParameterMark held=rp_mark();uint64_t held_live=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;
    CHECK(row_data_operation(&context,effects,NULL,NULL)==XR_XIR_OK);size_t sites=rp_attempts-attempts;CHECK(sites);
    rp_balanced(held);CHECK(rp_stats(&context).live_bytes==held_live);
    for(size_t at=0;at<sites;++at) {
        attempts=rp_attempts;rp_fail_at=attempts+at;rp_injected=false;XrCompileResources *identity=context.resources;
        XrXirStatus status=row_data_operation(&context,effects,NULL,NULL);
        rp_fail_at=SIZE_MAX;CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY && context.resources==identity);
        rp_balanced(held);CHECK(rp_stats(&context).live_bytes==held_live);
        CHECK(row_data_operation(&context,effects,NULL,NULL)==XR_XIR_OK && context.resources==identity);
        rp_balanced(held);CHECK(rp_stats(&context).live_bytes==held_live);
    }
    fprintf(stderr,"Physical row actual scoped allocation sites%zu: every ordinal entered OOM, same finite ledger retry and physical/live baseline PASS\n",sites);
    effect_body_material_free(other);xr_xir_compile_effects_free(effects);
    rp_owner_free(&context,live);rp_balanced(mark);row_data_axes();
}
#endif // XIR_INVOCATION_ROW_DATA_CASES_H
