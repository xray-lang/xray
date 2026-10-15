/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_small_runs.c - Complete short physical directories and failures
 */
#define main small_runs_retained_original_main
#include "test_xir_root_conditional_helpers.c"
#undef main

static void small_runs_reject(const EffectBodyRead *read,const EffectBodyFunction *function) {
    for (uint32_t i=0;i<function->instruction_count;++i) {
        const EffectBodyRun *sentinel=(const EffectBodyRun *)function,*output=sentinel;
        XrXirInstruction held={.op=XR_XIR_RETURN,.type=XR_XIR_F64,.immediate=INT64_MIN},row=held;
        CHECK(effect_body_record_ref(read,function,i,&output)==XR_XIR_BAD_STRUCTURE && output==sentinel);
        CHECK(effect_body_instruction_get(read,function,i,&row)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&row,&held,sizeof(row)));
    }
}

static void small_runs_case(uint32_t runs) {
    CHECK(runs>=1 && runs<=3);
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    XrXirInstruction operations[5]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2},
        {.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1},
        {.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL},
        {.op=XR_XIR_RETURN}};
    uint32_t count=runs*2-1;
    operations[count-1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    XrXirInstruction expected[5];memcpy(expected,operations,sizeof(expected));
    fixture.functions[3].instructions=operations;
    fixture.init_block.count=fixture.functions[3].instruction_count=count;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    memset(&fixture,0xCE,sizeof(fixture));memset(operations,0xCE,sizeof(operations));
    EffectBodyFunction *function=&effects->invocations->body->functions[3];
    CHECK(function->run_count==runs && function->instruction_count==count);
    EffectBodyRead read=effect_body_compile_read(&context);
    XrCompileResourceStats before=rp_stats(&context);
    for (uint32_t i=0;i<count;++i) {
        XrXirInstruction row={0};
        CHECK(effect_body_instruction_get(&read,function,i,&row)==XR_XIR_OK &&
            !memcmp(&row,&expected[i],sizeof(row)));
    }
    XrCompileResourceStats after=rp_stats(&context);
    CHECK(after.work>before.work && after.allocated_bytes==before.allocated_bytes &&
        after.live_bytes==before.live_bytes && after.peak_bytes==before.peak_bytes);
    for (uint32_t r=0;r<runs;++r) {
        EffectBodyRun saved=function->runs[r];
        function->runs[r].count=0;small_runs_reject(&read,function);function->runs[r]=saved;
        ++function->runs[r].first;small_runs_reject(&read,function);function->runs[r]=saved;
        function->runs[r].mask|=UINT32_C(1)<<10;
        small_runs_reject(&read,function);function->runs[r]=saved;
    }
    ++function->instruction_count;small_runs_reject(&read,function);--function->instruction_count;
    const EffectBodyRun *sentinel=(const EffectBodyRun *)function,*output=sentinel;
    CHECK(effect_body_record_ref(&read,function,count,&output)==XR_XIR_BAD_STRUCTURE && output==sentinel);
    for (uint32_t i=0;i<count;++i) {
        XrXirInstruction row={0};
        CHECK(effect_body_instruction_get(&read,function,i,&row)==XR_XIR_OK &&
            !memcmp(&row,&expected[i],sizeof(row)));
    }
    uint64_t work=rp_stats(&context).work,limit=rp_caps().work;
    CHECK(work<limit && xr_compile_resources_work(context.resources,limit-work-1)==XR_COMPILE_RESOURCE_OK);
    CHECK(effect_body_record_ref(&read,function,0,&output)==XR_XIR_BUDGET && output==sentinel);
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(!rp_live && !rp_live_bytes && !rp_allocation_observer);
}

int main(void) {
    for (uint32_t runs=1;runs<=3;++runs) small_runs_case(runs);
    puts("SMALL_RUNS actual owned directories1/2/3, all rows, every header/EOF, unchanged failure outputs, physical0 PASS");
    return 0;
}
