/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_borrowed_instruction_cases.h - Local complete rows and unchanged callbacks
 */
#ifndef XIR_BORROWED_INSTRUCTION_CASES_H
#define XIR_BORROWED_INSTRUCTION_CASES_H

static uint32_t borrowed_instruction_calls;
static XrXirStatus borrowed_instruction_delegate(const EffectBodyRequest *request,
    uint32_t f,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output,
    EffectBodyInstructionFields *fields) {
    ++borrowed_instruction_calls;
    return effect_body_owned_instruction(request,f,i,storage,output,fields);
}
/* The oracle is ten literal physical words, including unused targets and both
 * halves of a negative immediate. Inactive delta memory remains poisoned. */
static void borrowed_instruction_mask(uint32_t mask,uint32_t less,uint32_t position) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;
    EffectBodyRun run;memset(&run,0xCE,sizeof(run));run.first=0;run.count=2;run.mask=mask;
    run.base=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=17,.args={31,41},.targets={51,61},
        .type_arguments={81,91}};
    uint64_t immediate=UINT64_C(0x8000000112345678);memcpy(&run.base.immediate,&immediate,sizeof(immediate));
    for (uint32_t w=0;w<10;++w) if (mask&(UINT32_C(1)<<w)) run.delta[w]=w+1;
    EffectBodyFunction function={.instruction_count=2,.runs=&run,.run_count=1,.run_capacity=1};
    EffectBodyMaterial material={.resources=context.resources,.references=1,.functions=&function};
    material.domain=(XirMetadataDomain){.types=&material.terms.types,.function_count=1};
    EffectBodyRequest request=effect_body_owned_request(&context,&material);
    uint32_t selected=position?mask:0;
    XrXirInstruction expected={.op=(XrXirOp)(XR_XIR_CONST_INT+((selected&1)?1:0)),
        .type=17+((selected&2)?2:0),.args={31+((selected&4)?3:0),41+((selected&8)?4:0)},
        .targets={51+((selected&16)?5:0),61+((selected&32)?6:0)},
        .type_arguments={81+((selected&256)?9:0),91+((selected&512)?10:0)}};
    immediate=(UINT64_C(0x12345678)+((selected&64)?7:0))|
        ((UINT64_C(0x80000001)+((selected&128)?8:0))<<32);
    memcpy(&expected.immediate,&immediate,sizeof(immediate));
    XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_F64,.immediate=INT64_MIN},storage=sentinel;
    const XrXirInstruction *operation=NULL;XrCompileResourceStats before=rp_stats(&context);
    CHECK(effect_body_request_operation(&request,0,position,&storage,&operation)==XR_XIR_OK &&
        operation==(selected?&storage:&run.base) && !memcmp(operation,&expected,sizeof(expected)));
    uint64_t required=rp_stats(&context).work-before.work;CHECK(required);
    if (!selected) CHECK(!memcmp(&storage,&sentinel,sizeof(storage)));
    for (uint32_t w=0;w<10;++w) if (mask&(UINT32_C(1)<<w)) {
        uint32_t saved=run.delta[w];run.delta[w]=0;operation=(const XrXirInstruction *)&function;storage=sentinel;
        CHECK(effect_body_request_operation(&request,0,position,&storage,&operation)==XR_XIR_BAD_STRUCTURE &&
            operation==(const XrXirInstruction *)&function && !memcmp(&storage,&sentinel,sizeof(storage)));
        run.delta[w]=saved;operation=NULL;
        CHECK(effect_body_request_operation(&request,0,position,&storage,&operation)==XR_XIR_OK &&
            !memcmp(operation,&expected,sizeof(expected)));
    }
    EffectBodyRequest delegated=request;delegated.instruction=borrowed_instruction_delegate;
    operation=NULL;borrowed_instruction_calls=0;before=rp_stats(&context);
    CHECK(effect_body_request_operation(&delegated,0,position,&storage,&operation)==XR_XIR_OK &&
        operation==&storage && !memcmp(operation,&expected,sizeof(expected)) && borrowed_instruction_calls==1);
    uint64_t delegated_work=rp_stats(&context).work-before.work;
    operation=NULL;before=rp_stats(&context);
    CHECK(effect_body_request_instruction(&request,0,position,&storage,&operation)==XR_XIR_OK && operation==&storage &&
        !memcmp(operation,&expected,sizeof(expected)) && rp_stats(&context).work-before.work==delegated_work);
    if (!selected) CHECK(required<delegated_work);
    else CHECK(required==delegated_work+6);
    operation=(const XrXirInstruction *)&function;
    CHECK(effect_body_request_operation(&request,0,2,&storage,&operation)==XR_XIR_BAD_STRUCTURE &&
        operation==(const XrXirInstruction *)&function);
    CHECK(rp_attempts==attempts && rp_stats(&context).allocated_bytes==before.allocated_bytes &&
        rp_stats(&context).live_bytes==before.live_bytes);
    uint64_t work=rp_stats(&context).work,maximum=rp_caps().work;
    CHECK(work+required-less<maximum &&
        xr_compile_resources_work(context.resources,maximum-work-required+less)==XR_COMPILE_RESOURCE_OK);
    operation=(const XrXirInstruction *)&function;storage=sentinel;
    CHECK(effect_body_request_operation(&request,0,position,&storage,&operation)==(less?XR_XIR_BUDGET:XR_XIR_OK));
    if (less) CHECK(operation==(const XrXirInstruction *)&function);
    else CHECK(operation==(selected?&storage:&run.base) && !memcmp(operation,&expected,sizeof(expected)));
    if (!selected) CHECK(!memcmp(&storage,&sentinel,sizeof(storage)));
    rp_owner_free(&context,baseline);rp_balanced(physical);CHECK(!rp_live && !rp_live_bytes);
}

/* A literal remaining budget refuses each prefix of the real consume sequence.
 * Each owner retains the original limit; no rejected charge is refunded. */
static void borrowed_instruction_refusals(uint32_t remaining) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;
    EffectBodyRun run={.count=2,.base={.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=INT64_MIN}};
    EffectBodyFunction function={.instruction_count=2,.runs=&run,.run_count=1,.run_capacity=1};
    EffectBodyMaterial material={.resources=context.resources,.references=1,.functions=&function};
    material.domain=(XirMetadataDomain){.types=&material.terms.types,.function_count=1};
    EffectBodyRequest request=effect_body_owned_request(&context,&material);
    XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL},storage=sentinel;
    const XrXirInstruction *operation=NULL;uint64_t work=rp_stats(&context).work;
    CHECK(effect_body_request_operation(&request,0,1,&storage,&operation)==XR_XIR_OK && operation==&run.base);
    uint64_t required=rp_stats(&context).work-work;
    if (remaining<required) {
        work=rp_stats(&context).work;
        CHECK(xr_compile_resources_work(context.resources,rp_caps().work-work-remaining)==XR_COMPILE_RESOURCE_OK);
        operation=(const XrXirInstruction *)&function;
        CHECK(effect_body_request_operation(&request,0,1,&storage,&operation)==XR_XIR_BUDGET &&
            operation==(const XrXirInstruction *)&function && !memcmp(&storage,&sentinel,sizeof(storage)));
    }
    CHECK(required<128 && rp_attempts==attempts);
    rp_owner_free(&context,baseline);rp_balanced(physical);CHECK(!rp_live && !rp_live_bytes);
}

static void borrowed_instruction_binding(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;
    EffectBodyRun run={.count=2,.base={.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42}};
    EffectBodyFunction function={.instruction_count=2,.runs=&run,.run_count=1,.run_capacity=1};
    EffectBodyMaterial material={.resources=context.resources,.references=1,.functions=&function};
    material.domain=(XirMetadataDomain){.types=&material.terms.types,.function_count=1};
    EffectBodyRequest request=effect_body_owned_request(&context,&material);
    for (uint32_t damage=0;damage<14;++damage) {
        EffectBodyRequest changed=request;EffectBodyFunction saved_function=function;EffectBodyRun saved=run;
        if (!damage) changed.domain.types=&effect_empty_types;
        else if (damage==1) ++changed.domain.function_count;
        else if (damage==2) changed.domain.declarations=(const XrXirDeclarations *)&material;
        else if (damage==3) changed.domain.generics=(const XrXirGeneric *)&material;
        else if (damage==4) changed.domain.defaults=(const XrXirDefaultTable *)&material;
        else if (damage==5) run.first=1;
        else if (damage==6) run.count=0;
        else if (damage==7) run.count=3;
        else if (damage==8) run.mask=1024;
        else if (damage==9) function.runs=NULL;
        else if (damage==10) function.run_capacity=0;
        else if (damage==11) run.base.op=XR_XIR_INVALID;
        else if (damage==12) run.base.op=XR_XIR_OP_COUNT;
        else material.resources=NULL;
        XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL},storage=sentinel;
        const XrXirInstruction *operation=(const XrXirInstruction *)&function;
        CHECK(effect_body_request_operation(&changed,0,1,&storage,&operation)==XR_XIR_BAD_STRUCTURE &&
            operation==(const XrXirInstruction *)&function && !memcmp(&storage,&sentinel,sizeof(storage)));
        function=saved_function;run=saved;material.resources=context.resources;operation=NULL;
        CHECK(effect_body_request_operation(&request,0,1,&storage,&operation)==XR_XIR_OK && operation==&run.base &&
            operation->immediate==42 && !memcmp(&storage,&sentinel,sizeof(storage)));
    }
    /* Raw callers keep their genuine physical pointer and original fee. */
    XrXirInstruction row={.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    XrXirFunction raw_function={.instructions=&row,.instruction_count=1};
    XrXirModule module={.functions=&raw_function,.function_count=1};
    EffectBodyRequest raw=effect_body_raw_request(&context,&module);
    XrXirInstruction storage={0};const XrXirInstruction *operation=NULL;XrCompileResourceStats before=rp_stats(&context);
    CHECK(effect_body_request_operation(&raw,0,0,&storage,&operation)==XR_XIR_OK && operation==&row &&
        rp_stats(&context).work-before.work==2+sizeof(operation) && rp_attempts==attempts);
    rp_owner_free(&context,baseline);rp_balanced(physical);CHECK(!rp_live && !rp_live_bytes);
}
static void borrowed_instruction_cases(void) {
    for (uint32_t mask=0;mask<1024;++mask) for (uint32_t less=0;less<2;++less)
        for (uint32_t position=0;position<2;++position) borrowed_instruction_mask(mask,less,position);
    for (uint32_t remaining=0;remaining<128;++remaining) borrowed_instruction_refusals(remaining);
    borrowed_instruction_binding();
    puts("BORROWED_ROW all1024 literal masks/exact-minus1,complete unused fields/negative immediate,delegated/raw fees,current binding/restores,all budget prefixes,no allocations,physical0 PASS");
}
#endif // XIR_BORROWED_INSTRUCTION_CASES_H
