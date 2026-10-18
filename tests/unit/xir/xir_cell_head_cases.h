/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_head_cases.h - Current owner projections and exact work boundaries
 */
#ifndef XIR_CELL_HEAD_CASES_H
#define XIR_CELL_HEAD_CASES_H

static uint32_t cell_head_delegate_calls;
static XrXirStatus cell_head_delegate(const EffectBodyRequest *request,uint32_t f,
    EffectBodyFunctionHead *output,EffectBodyFunctionDimensions *dimensions,EffectBodyFunctionSignature *signature) {
    ++cell_head_delegate_calls;
    return effect_body_owned_head(request,f,output,dimensions,signature);
}
static void cell_head_equal(const XrXirCellProofBodyHead *a,const EffectBodyFunction *b) {
    CHECK(a->parameters==b->parameters && a->parameter_count==b->parameter_count &&
        a->result==b->result && a->instruction_count==b->instruction_count &&
        a->operands==b->operands && a->operand_count==b->operand_count);
}
static void cell_head_refuse(const XrXirCompileContext *context,
    const EffectCellBodyAdapter *adapter,uint32_t f) {
    XrXirCellProofBodyHead held,row;memset(&held,0xA5,sizeof(held));row=held;
    CHECK(effect_cell_body_head(context,adapter,f,&row)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&held,&row,sizeof(row)));
}
static void cell_head_case(uint32_t selected,uint32_t less) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    EffectBodyRequest raw=effect_body_raw_request(&context,&fixture.module);
    EffectCellBodyAdapter raw_adapter={0};XrXirCellProofBodyRead raw_read={0};
    CHECK(effect_cell_body_request(&context,&raw,&raw_adapter,&raw_read)==XR_XIR_OK);
    XrXirCellProofBodyHead row={0};XrCompileResourceStats before=rp_stats(&context);
    CHECK(raw_read.head(&context,raw_read.owner,1,&row)==XR_XIR_OK);
    XrCompileResourceStats after=rp_stats(&context);
    CHECK(row.parameter_count==2 && row.parameters==fixture.parameters && row.result==XR_XIR_I64 &&
        row.instruction_count==2 && row.operand_count==1 && row.operands==&fixture.reference);
    uint64_t original=11+11+sizeof(EffectBodyFunctionHead)+6+sizeof(row);
    CHECK(after.work-before.work==original && after.allocated_bytes==before.allocated_bytes);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    EffectBodyMaterial *body=effects->invocations->body;
    EffectBodyRequest request=effect_body_owned_request(&context,body);
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    CHECK(effect_cell_body_request(&context,&request,&adapter,&read)==XR_XIR_OK);
    size_t attempts=rp_attempts;before=rp_stats(&context);
    CHECK(read.head(&context,read.owner,selected,&row)==XR_XIR_OK);
    after=rp_stats(&context);cell_head_equal(&row,&body->functions[selected]);
    CHECK(after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes &&
        after.peak_bytes==before.peak_bytes && rp_attempts==attempts);
    uint64_t required=after.work-before.work;
    CHECK(required==11+6+17+sizeof(row) && required<original);
    EffectCellBodyAdapter delegated=adapter;delegated.request.head=cell_head_delegate;
    cell_head_delegate_calls=0;before=rp_stats(&context);
    CHECK(effect_cell_body_head(&context,&delegated,selected,&row)==XR_XIR_OK);
    after=rp_stats(&context);cell_head_equal(&row,&body->functions[selected]);
    CHECK(cell_head_delegate_calls==1 && after.work-before.work==original);
    EffectBodyFunction *head=&body->functions[1],saved=*head;
    head->name=NULL;cell_head_refuse(&context,&adapter,1);*head=saved;
    head->parameters=NULL;cell_head_refuse(&context,&adapter,1);*head=saved;
    head->blocks=NULL;cell_head_refuse(&context,&adapter,1);*head=saved;
    head->operands=NULL;cell_head_refuse(&context,&adapter,1);*head=saved;
    head->runs=NULL;cell_head_refuse(&context,&adapter,1);*head=saved;
    head->run_count=0;cell_head_refuse(&context,&adapter,1);*head=saved;
    head->run_capacity=0;cell_head_refuse(&context,&adapter,1);*head=saved;
    cell_head_refuse(&context,&adapter,body->domain.function_count);
    EffectCellBodyAdapter damaged=adapter;damaged.request.domain.types=&effect_empty_types;
    cell_head_refuse(&context,&damaged,1);damaged=adapter;
    damaged.request.domain.declarations=NULL;cell_head_refuse(&context,&damaged,1);damaged=adapter;
    damaged.request.domain.function_count++;cell_head_refuse(&context,&damaged,1);damaged=adapter;
    damaged.request.domain.generics=(const XrXirGeneric *)body;
    cell_head_refuse(&context,&damaged,1);damaged=adapter;
    damaged.request.domain.defaults=(const XrXirDefaultTable *)body;
    cell_head_refuse(&context,&damaged,1);
    XrCompileResources *resources=body->resources;body->resources=NULL;
    cell_head_refuse(&context,&adapter,1);body->resources=resources;
    CHECK(rp_attempts==attempts);
    uint64_t work=rp_stats(&context).work,limit=rp_caps().work,remaining=required-less;
    CHECK(work+remaining<limit &&
        xr_compile_resources_work(context.resources,limit-work-remaining)==XR_COMPILE_RESOURCE_OK);
    XrXirCellProofBodyHead held;memset(&held,0xA5,sizeof(held));row=held;
    XrXirStatus status=effect_cell_body_head(&context,&adapter,selected,&row);
    CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK));
    if (less) CHECK(!memcmp(&row,&held,sizeof(row)));
    else cell_head_equal(&row,&body->functions[selected]);
    CHECK(rp_attempts==attempts);
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(!rp_live && !rp_live_bytes && !rp_allocation_observer);
}
static void cell_head_cases(void) {
    for (uint32_t f=0;f<4;++f) for (uint32_t less=0;less<2;++less) cell_head_case(f,less);
    puts("CELL_HEAD current owner/domain/all head storage, raw/delegated fees, exact/minus1, producer-dead and physical0 PASS");
}
#endif // XIR_CELL_HEAD_CASES_H
