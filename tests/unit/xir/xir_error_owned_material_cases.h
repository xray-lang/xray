/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_owned_material_cases.h - Error replay after real producer death
 *
 * KEY CONCEPT:
 *   The genuine sealed certificate owns every CFG input and authority proof.
 */
#ifndef XIR_ERROR_OWNED_MATERIAL_CASES_H
#define XIR_ERROR_OWNED_MATERIAL_CASES_H

static unsigned owned_error_refusals;
static XrXirStatus owned_error_refuse_head(const EffectBodyRequest *read,uint32_t function,
    EffectBodyFunctionHead *head,EffectBodyFunctionDimensions *dimensions) {
    (void)read;(void)function;(void)head;(void)dimensions;
    ++owned_error_refusals;return XR_XIR_BAD_STRUCTURE;
}

static void owned_error_input_controls(const XrXirCompileContext *context,
    XrXirEffects *effects,ErrorFlow *flow) {
    ErrorInvocationContexts *sentinel=(ErrorInvocationContexts *)(uintptr_t)1;
    XrCompileResourceStats before=rp_stats(context);size_t attempts=rp_attempts;
    CHECK(error_invocation_contexts_new(flow,&sentinel)==XR_XIR_BAD_STRUCTURE &&
        sentinel==(ErrorInvocationContexts *)(uintptr_t)1);
    CHECK(rp_stats(context).work==before.work && rp_attempts==attempts);
    ErrorInvocationContexts *output=NULL;EffectBodyRequest saved=flow->body;
    flow->body.head=owned_error_refuse_head;owned_error_refusals=0;
    CHECK(error_invocation_contexts_new(flow,&output)==XR_XIR_BAD_STRUCTURE &&
        !output && owned_error_refusals==1);
    flow->body=saved;
    flow->body.domain.function_count++;
    CHECK(error_invocation_contexts_new(flow,&output)==XR_XIR_BAD_STRUCTURE && !output);
    flow->body=saved;
    flow->body.owner=NULL;
    CHECK(error_invocation_contexts_new(flow,&output)==XR_XIR_BAD_STRUCTURE && !output);
    flow->body=saved;
    XrXirCompileContext foreign=rp_owner(rp_caps());uint64_t baseline=rp_stats(&foreign).live_bytes;
    XrXirCompileContext *remaining=flow->remaining;flow->remaining=&foreign;
    before=rp_stats(&foreign);attempts=rp_attempts;
    CHECK(error_invocation_contexts_new(flow,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(rp_stats(&foreign).work==before.work && rp_attempts==attempts && effects->resources==context->resources);
    flow->remaining=remaining;rp_owner_free(&foreign,baseline);
}

static void owned_error_source_controls(const XrXirCompileContext *context,
    const EffectBodyMaterial *body,const XrXirModule *original) {
    CHECK(effect_body_source_match(context,body,original)==XR_XIR_OK);
    XrXirFunction functions[5];XrXirInstruction operations[4];
    CHECK(original->function_count==5 && original->functions[2].instruction_count<=4);
    memcpy(functions,original->functions,sizeof(functions));
    memcpy(operations,functions[2].instructions,functions[2].instruction_count*sizeof(*operations));
    XrXirModule changed=*original;changed.functions=functions;functions[2].instructions=operations;
    operations[0].targets[0]^=1;
    CHECK(effect_body_source_match(context,body,&changed)==XR_XIR_BAD_STRUCTURE);
    operations[0].targets[0]^=1;operations[0].immediate^=INT64_C(1)<<32;
    CHECK(effect_body_source_match(context,body,&changed)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_body_source_match(context,body,original)==XR_XIR_OK);
}

/* The pure returned target and the non-producing factory retain independent
 * known and UNKNOWN error outcomes after both producers have been destroyed. */
static XrXirStatus conditional_owned_error(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    InvocationReturnedFixture fixture;invocation_returned_fixture(&fixture,mode?4:0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    if (status==XR_XIR_OK && oracle)
        owned_error_source_controls(context,effects->invocations->body,xr_xir_compile_artifact_module(checked));
    xr_xir_compile_artifact_free(checked);checked=NULL;memset(&fixture,0xCE,sizeof(fixture));
    EffectTerms terms={0};terms.remaining=context;ErrorFlow flow={0};
    if (status==XR_XIR_OK) {
        flow.body=effect_body_owned_request(context,effects->invocations->body);
        terms.types=*flow.body.domain.types;flow.effects=effects;
        flow.remaining=(XrXirCompileContext *)context;flow.terms=&terms;
        flow.summary_count=effects->count;flow.invocation=UINT32_MAX;
        if (oracle) owned_error_input_controls(context,effects,&flow);
        status=error_invocation_contexts_new(&flow,&flow.invocations);
    }
    if (status==XR_XIR_OK && !flow.invocations) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) {
        size_t bytes=(size_t)effects->count*effects->words*sizeof(*effects->errors);
        if (!xir_compile_work(context,bytes)) status=XR_XIR_BUDGET;
        else memset(effects->errors,0,bytes);
    }
    if (status==XR_XIR_OK) status=error_invocation_summaries(&flow);
    if (status==XR_XIR_OK) status=error_invocation_functions(&flow);
    if (status==XR_XIR_OK) {
        CHECK(effects->count==5 && effects->errors && effects->words==1);
        CHECK(error_bit(effects->errors,0)==(mode!=0) && !error_bit(effects->errors,1));
        CHECK(!effects->invocations->equations->module && !effects->invocations->equations->work);
    }
    error_storage_free(&flow);error_invocation_contexts_free(flow.invocations);
    effect_terms_free(&terms);xr_xir_compile_effects_free(effects);return status;
}

static void conditional_owned_error_literals(void) {
    for (uint32_t mode=0;mode<2;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(conditional_owned_error(&context,mode,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}

/* Only the original complete compiler lane runs this extra two-operation
 * census. No focused ordinary argv repeats its full all-ordinal fault work. */
static void conditional_owned_error_resources(void) {
    for (uint32_t mode=0;mode<2;++mode) {
        size_t sites=0;XrCompileResourceStats census={0},entry={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());entry=rp_stats(&context);
            RootParameterMark retained=rp_mark();size_t first=rp_attempts;
            rp_fail_at=pass?first+pass-1:SIZE_MAX;
            XrXirStatus status=conditional_owned_error(&context,mode,false);
            if (!pass) {
                CHECK(status==XR_XIR_OK);sites=rp_attempts-first;census=rp_stats(&context);CHECK(sites);
                printf("OWNED_ERROR_CENSUS mode=%u sites=%zu operation_work=%llu operation_bytes=%llu owner_work=%llu owner_bytes=%llu peak=%llu allocations=%llu\n",
                    mode,sites,(unsigned long long)(census.work-entry.work),
                    (unsigned long long)(census.allocated_bytes-entry.allocated_bytes),
                    (unsigned long long)census.work,(unsigned long long)census.allocated_bytes,
                    (unsigned long long)census.peak_bytes,
                    (unsigned long long)(census.allocation_count-entry.allocation_count));
            } else {
                CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts-first==pass);
                CHECK(rp_stats(&context).live_bytes==entry.live_bytes);rp_balanced(retained);
                XrCompileResourceStats consumed=rp_stats(&context);
                uint64_t work=census.work-entry.work,bytes=census.allocated_bytes-entry.allocated_bytes;
                bool fits=work<=context.resources->limits.work-consumed.work &&
                    bytes<=context.resources->limits.allocated_bytes-consumed.allocated_bytes;
                rp_fail_at=SIZE_MAX;size_t retry=rp_attempts;
                status=conditional_owned_error(&context,mode,false);
                CHECK(status==(fits?XR_XIR_OK:XR_XIR_BUDGET));
                CHECK(rp_stats(&context).live_bytes==entry.live_bytes);rp_balanced(retained);
                if (fits) CHECK(rp_stats(&context).work-consumed.work==work &&
                    rp_stats(&context).allocated_bytes-consumed.allocated_bytes==bytes &&
                    rp_stats(&context).allocation_count-consumed.allocation_count==census.allocation_count-entry.allocation_count &&
                    rp_attempts-retry==sites);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
        }
        uint64_t measured[3]={census.allocated_bytes,census.peak_bytes,census.work};
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=rp_caps();uint64_t limit=measured[axis]-minus;
            if (axis==0) limits.allocated_bytes=limit;
            else if (axis==1) limits.live_bytes=limit;
            else limits.work=limit;
            RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(limits);
            uint64_t baseline=rp_stats(&context).live_bytes;
            CHECK(conditional_owned_error(&context,mode,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
            rp_owner_free(&context,baseline);rp_balanced(physical);
        }
    }
}
#endif // XIR_ERROR_OWNED_MATERIAL_CASES_H
