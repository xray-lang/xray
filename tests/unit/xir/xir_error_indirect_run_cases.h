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

static XrXirStatus error_census_scalar(ErrorFlow *flow,bool *output) {
    bool indirect=false;
    for(uint32_t f=0;f<flow->body.domain.function_count && !indirect;++f) {
        EffectBodyFunctionDimensions head={0};
        XrXirStatus read=effect_body_request_dimensions(&flow->body,f,&head);
        if(read!=XR_XIR_OK)return read;
        for(uint32_t i=0;i<head.instruction_count;++i) {
            if(!xir_compile_work(flow->remaining,1))return XR_XIR_BUDGET;
            EffectBodyInstructionShape shape={0};read=error_shape(flow,f,i,&shape);
            if(read!=XR_XIR_OK)return read;
            if(shape.op==XR_XIR_CALL_INDIRECT || shape.op==XR_XIR_INVOKE_INDIRECT){indirect=true;break;}
        }
    }
    *output=indirect;return XR_XIR_OK;
}
static uint32_t error_census_reads,error_census_refuse=UINT32_MAX;
static XrXirStatus error_census_delegate(const EffectBodyRequest *request,uint32_t f,uint32_t i,
    EffectBodyInstructionShape *output) {
    ++error_census_reads;
    if(f==3 && i==error_census_refuse)return XR_XIR_BAD_STRUCTURE;
    return effect_body_owned_shape(request,f,i,output);
}
static void error_census_reject(ErrorFlow *flow,XrXirStatus status) {
    bool output=true;
    CHECK(error_invocation_has_indirect(flow,&output)==status && output);
}
static void error_census_owned(uint32_t runs,bool present,uint32_t less) {
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
    ErrorFlow raw={.body=effect_body_raw_request(&context,&fixture.module),.remaining=&context};
    bool got=!present,old=!present;
    CHECK(error_invocation_has_indirect(&raw,&got)==XR_XIR_OK && got==present);
    CHECK(error_census_scalar(&raw,&old)==XR_XIR_OK && old==present);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));memset(ops,0xCE,sizeof(ops));memset(varied,0xCE,sizeof(varied));memset(&varied_block,0xCE,sizeof(varied_block));memset(varied_operands,0xCE,sizeof(varied_operands));
    EffectBodyMaterial *body=effects->invocations->body;CHECK(body->functions[3].run_count==runs);
    ErrorFlow flow={.body=effect_body_owned_request(&context,body),.remaining=&context};
    got=!present;old=!present;XrCompileResourceStats before=rp_stats(&context);size_t attempts=rp_attempts;
    CHECK(error_invocation_has_indirect(&flow,&got)==XR_XIR_OK && got==present);
    XrCompileResourceStats after=rp_stats(&context);
    CHECK(after.work>before.work && rp_attempts==attempts && after.allocated_bytes==before.allocated_bytes &&
        after.live_bytes==before.live_bytes && after.peak_bytes==before.peak_bytes);
    CHECK(error_census_scalar(&flow,&old)==XR_XIR_OK && old==present);
    ErrorFlow delegated=flow;delegated.body.shape=error_census_delegate;error_census_reads=0;error_census_refuse=UINT32_MAX;
    got=!present;CHECK(error_invocation_has_indirect(&delegated,&got)==XR_XIR_OK && got==present);uint32_t visits=error_census_reads;
    error_census_reads=0;old=!present;CHECK(error_census_scalar(&delegated,&old)==XR_XIR_OK && old==present && error_census_reads==visits);
    error_census_refuse=0;error_census_reads=0;
    got=true;XrXirStatus status=error_invocation_has_indirect(&delegated,&got);
    CHECK(status==(present?XR_XIR_OK:XR_XIR_BAD_STRUCTURE) && got);
    CHECK(present || error_census_reads==body->functions[0].instruction_count+body->functions[1].instruction_count+body->functions[2].instruction_count+1);
    error_census_refuse=UINT32_MAX;
    if(present) {
        /* Found opcodes do not publish before this complete function's EOF. */
        ++body->functions[1].instruction_count;bool unchanged=false;
        CHECK(error_invocation_has_indirect(&flow,&unchanged)==XR_XIR_BAD_STRUCTURE && !unchanged);
        --body->functions[1].instruction_count;
    }
    if(!present) {
        EffectBodyFunction *function=&body->functions[3];
        for(uint32_t r=0;r<runs;++r) {
            EffectBodyRun saved=function->runs[r];
            function->runs[r].count=0;error_census_reject(&flow,XR_XIR_BAD_STRUCTURE);function->runs[r]=saved;
            ++function->runs[r].first;error_census_reject(&flow,XR_XIR_BAD_STRUCTURE);function->runs[r]=saved;
            function->runs[r].mask|=1024;error_census_reject(&flow,XR_XIR_BAD_STRUCTURE);function->runs[r]=saved;
            function->runs[r].base.op=XR_XIR_OP_COUNT;error_census_reject(&flow,XR_XIR_BAD_STRUCTURE);function->runs[r]=saved;
        }
        if(runs>1) {
            EffectBodyRun saved=function->runs[0];
            for(uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
                function->runs[0].mask|=UINT32_C(1)<<w;function->runs[0].delta[w]=0;
                error_census_reject(&flow,XR_XIR_BAD_STRUCTURE);function->runs[0]=saved;
            }
            function->runs[0].base.op=(XrXirOp)(XR_XIR_INVALID+1);function->runs[0].mask|=1;function->runs[0].delta[0]=UINT32_MAX;
            error_census_reject(&flow,XR_XIR_BAD_STRUCTURE);function->runs[0]=saved;
        }
        ++function->instruction_count;error_census_reject(&flow,XR_XIR_BAD_STRUCTURE);--function->instruction_count;
        /* This is a storage-only empty directory on a genuine retained owner. */
        EffectBodyFunction saved_function=*function;
        *function=(EffectBodyFunction){.result=XR_XIR_UNIT};
        bool empty=true;CHECK(error_invocation_has_indirect(&flow,&empty)==XR_XIR_OK && !empty);
        function->run_capacity=1;error_census_reject(&flow,XR_XIR_BAD_STRUCTURE);
        *function=saved_function;
    }
    ErrorFlow foreign=flow;foreign.body.domain.function_count++;error_census_reject(&foreign,XR_XIR_BAD_STRUCTURE);
    foreign=flow;foreign.body.domain.types=NULL;error_census_reject(&foreign,XR_XIR_BAD_STRUCTURE);
    foreign=flow;foreign.body.domain.defaults=NULL;
    if(flow.body.domain.defaults)error_census_reject(&foreign,XR_XIR_BAD_STRUCTURE);
    XrXirCompileContext other=rp_owner(rp_caps());foreign=flow;foreign.remaining=&other;
    XrCompileResourceStats other_before=rp_stats(&other);error_census_reject(&foreign,XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats other_after=rp_stats(&other);CHECK(!memcmp(&other_before,&other_after,sizeof(other_before)));rp_owner_free(&other,other_before.live_bytes);
    before=rp_stats(&context);got=!present;CHECK(error_invocation_has_indirect(&flow,&got)==XR_XIR_OK && got==present);after=rp_stats(&context);
    uint64_t required=after.work-before.work,limit=rp_caps().work,remaining=required-less;
    CHECK(required && after.work+remaining<limit && xr_compile_resources_work(context.resources,limit-after.work-remaining)==XR_COMPILE_RESOURCE_OK);
    got=!present;status=error_invocation_has_indirect(&flow,&got);
    CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK) && got==(less?!present:present));
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(!rp_live && !rp_live_bytes && !rp_allocation_observer);
}
static void error_indirect_run_cases(void) {
    for(uint32_t runs=1;runs<=5;++runs)
        for(uint32_t present=0;present<2;++present)
            for(uint32_t less=0;less<2;++less) error_census_owned(runs,present!=0,less);
}
#endif // XIR_ERROR_INDIRECT_RUN_CASES_H
