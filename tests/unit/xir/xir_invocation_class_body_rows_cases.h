/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_class_body_rows_cases.h - Complete current Class body replay
 */
#ifndef XIR_INVOCATION_CLASS_BODY_ROWS_CASES_H
#define XIR_INVOCATION_CLASS_BODY_ROWS_CASES_H
static size_t class_body_rows_reads,class_body_rows_refused=SIZE_MAX;
static size_t class_body_rows_type_sites,class_body_rows_instruction_sites;
static XrXirStatus class_body_rows_type(const EffectBodyRequest *request,uint32_t function,
    uint32_t value,XrXirType *output) {
    if (class_body_rows_reads++==class_body_rows_refused) return XR_XIR_BAD_STRUCTURE;
    return effect_body_owned_type(request,function,value,output);
}
static XrXirStatus class_body_rows_instruction(const EffectBodyRequest *request,uint32_t function,
    uint32_t ordinal,XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    if (class_body_rows_reads++==class_body_rows_refused) return XR_XIR_BAD_STRUCTURE;
    return effect_body_owned_instruction(request,function,ordinal,storage,output,fields);
}

/* Both delegated readers retain every real callback and first refusal.
 * No saved successful value supplies any subsequent replay. */
static void class_body_rows_reader_controls(EffectInvocationFlow *flow,uint64_t limit) {
    EffectBodyRequest original=flow->body;RootParameterMark held=rp_mark();
    uint64_t live=rp_stats(flow->owner->work).live_bytes;
    for (unsigned reader=0;reader<2;++reader) {
        flow->body=original;
        if (!reader) flow->body.type=class_body_rows_type;else flow->body.instruction=class_body_rows_instruction;
        class_body_rows_reads=0;class_body_rows_refused=SIZE_MAX;
        CHECK(effect_invocation_class_body_match(flow,limit)==XR_XIR_OK);
        size_t sites=class_body_rows_reads;CHECK(sites>=(!reader?flow->values:flow->function->instruction_count));
        if (!reader) class_body_rows_type_sites=sites;else class_body_rows_instruction_sites=sites;
        for (size_t at=0;at<sites;++at) {
            class_body_rows_reads=0;class_body_rows_refused=at;
            CHECK(effect_invocation_class_body_match(flow,limit)==XR_XIR_BAD_STRUCTURE && class_body_rows_reads==at+1);
            rp_balanced(held);CHECK(rp_stats(flow->owner->work).live_bytes==live);
        }
        class_body_rows_refused=SIZE_MAX;
    }
    flow->body=original;rp_balanced(held);
}

static void class_body_rows_current_controls(EffectInvocationFlow *flow,uint64_t limit) {
    EffectInvocationWorkspace *owner=flow->owner;const XrXirCompileContext *work=owner->work;
    const EffectInvocationNode *node=&owner->final.nodes[flow->node];CHECK(node->class_count && node->classes);
    XrXirType class_type=flow->types[node->classes[0].value];CHECK(xr_xir_type_is_class(flow->body.domain.types,class_type));
    RootParameterMark held=rp_mark();uint64_t live=rp_stats(work).live_bytes;
    for (uint32_t v=0;v<flow->values;++v) {
        XrXirType saved=flow->types[v];flow->types[v]=xr_xir_type_is_class(flow->body.domain.types,saved)?XR_XIR_I64:class_type;
        CHECK(effect_invocation_class_body_match(flow,limit)==XR_XIR_BAD_STRUCTURE);flow->types[v]=saved;rp_balanced(held);
    }
    uint32_t returns=0;
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        XrXirInstruction *row=(XrXirInstruction *)(flow->instruction_refs?flow->instruction_refs[i]:&flow->instructions[i]);
        if (row->op!=XR_XIR_RETURN) continue;
        uint32_t saved=row->args[0];row->args[0]=UINT32_MAX;
        CHECK(effect_invocation_class_body_match(flow,limit)==XR_XIR_BAD_STRUCTURE);row->args[0]=saved;++returns;rp_balanced(held);
    }
    CHECK(returns && rp_stats(work).live_bytes==live);
    EffectBodyRequest original=flow->body;EffectBodyMaterial *material=owner->final.material;
    owner->final.material=NULL;CHECK(effect_invocation_class_body_match(flow,limit)==XR_XIR_BAD_STRUCTURE);owner->final.material=material;
    flow->body.domain.types=NULL;CHECK(effect_invocation_class_body_match(flow,limit)==XR_XIR_BAD_STRUCTURE);flow->body=original;
    ++flow->body.domain.function_count;CHECK(effect_invocation_class_body_match(flow,limit)==XR_XIR_BAD_STRUCTURE);flow->body=original;
    XrXirType *types=flow->types;flow->types=NULL;
    CHECK(effect_invocation_class_body_match(flow,limit)==XR_XIR_BAD_STRUCTURE);flow->types=types;
    const XrXirInstruction **refs=flow->instruction_refs;XrXirInstruction *instructions=flow->instructions;
    flow->instruction_refs=NULL;flow->instructions=NULL;
    CHECK(effect_invocation_class_body_match(flow,limit)==XR_XIR_BAD_STRUCTURE);flow->instruction_refs=refs;flow->instructions=instructions;
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats before=rp_stats(&foreign);size_t attempts=rp_attempts;
    flow->body.read.owner=&foreign;CHECK(effect_invocation_class_body_match(flow,limit)==XR_XIR_BAD_STRUCTURE);flow->body=original;
    CHECK(rp_attempts==attempts && rp_stats(&foreign).allocated_bytes==before.allocated_bytes &&
        rp_stats(&foreign).live_bytes==before.live_bytes && rp_stats(&foreign).work==before.work);
    rp_owner_free(&foreign,before.live_bytes);rp_balanced(held);
    CHECK(effect_invocation_class_body_match(flow,limit)==XR_XIR_OK && rp_stats(work).live_bytes==live);
}

/* A genuine complete Class certificate owns the source after the Checked
 * producer dies. Fresh independent origins precede the body reader controls. */
static XrXirStatus class_body_rows_run(const XrXirCompileContext *context,bool controls) {
    InvocationClassRowsFixture fixture;invocation_class_rows_fixture(&fixture,6);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;EffectInvocationWorkspace replay={0};EffectInvocationFlow flow={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if (status==XR_XIR_OK) {
        EffectInvocationCertificate *certificate=effects->invocations;CHECK(certificate && certificate->final);
        status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&replay);
        EffectBodyRequest body=effect_body_owned_request(context,certificate->body);
        if (status==XR_XIR_OK) {
            flow=(EffectInvocationFlow){.owner=&replay,.node=replay.final.roots[3],.instruction=UINT32_MAX,.precision=true};
            status=effect_invocation_origins_read(&flow,&body);
        }
        uint64_t limit=0;
        if (status==XR_XIR_OK) status=effect_invocation_class_rank_limit_count(&replay,body.domain.function_count,&limit);
        if (status==XR_XIR_OK) status=effect_invocation_class_body_match(&flow,limit);
        if (status==XR_XIR_OK && controls) {
            CHECK(flow.types_ready && flow.values && flow.function->instruction_count && replay.class_enabled);
            class_body_rows_reader_controls(&flow,limit);class_body_rows_current_controls(&flow,limit);
        }
    }
    xr_compile_resources_free(flow.memory);effect_invocation_read_close(&replay);xr_xir_compile_effects_free(effects);return status;
}

static void invocation_class_body_rows_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(class_body_rows_run(&context,false)==XR_XIR_OK);XrCompileResourceStats measured=rp_stats(&context);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
    CHECK(class_body_rows_run(&context,true)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        physical=rp_mark();XrCompileResourceLimits limits=rp_caps();
        if (!axis) limits.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=measured.peak_bytes-minus;else limits.work=measured.work-minus;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(class_body_rows_run(&context,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    fprintf(stderr,"Class current rows: genuine complete certificate after Checked/producer death and fresh independent origins, every type-word classification and real RETURN source refusal, current material/domain/foreign-ledger/missing-view gates; all%zu type and%zu instruction delegated callback refusals, whole genuine-fixture six axes%llu/%llu/%llu exact/minusone; no new allocation or semantic cache PASS\n",
        class_body_rows_type_sites,class_body_rows_instruction_sites,(unsigned long long)measured.allocated_bytes,
        (unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_CLASS_BODY_ROWS_CASES_H
