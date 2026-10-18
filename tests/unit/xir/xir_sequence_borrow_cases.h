/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sequence_borrow_cases.h - Complete ordered rows and distinct storage lifetimes
 *
 * KEY CONCEPT:
 *   Borrowing changes only the local row lifetime. Every physical field,
 *   current binding, sequential position and EOF remains independently checked.
 */
#ifndef XIR_SEQUENCE_BORROW_CASES_H
#define XIR_SEQUENCE_BORROW_CASES_H

static uint32_t sequence_borrow_delegate_calls;
static XrXirStatus sequence_borrow_delegate(const EffectBodyRequest *request,
    uint32_t f,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output,
    EffectBodyInstructionFields *fields) {
    ++sequence_borrow_delegate_calls;
    return effect_body_owned_instruction(request,f,i,storage,output,fields);
}

/* These literal words include unused fields and a negative immediate.
 * Poisoned inactive deltas must never affect either complete reader. */
static void sequence_borrow_mask(uint32_t mask,uint32_t less) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;
    EffectBodyRun run;memset(&run,0xCE,sizeof(run));run.first=0;run.count=2;run.mask=mask;
    run.base=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=17,.args={31,41},.targets={51,61},
        .type_arguments={81,91}};
    uint64_t immediate=UINT64_C(0x8000000112345678);memcpy(&run.base.immediate,&immediate,sizeof(immediate));
    for(uint32_t w=0;w<10;++w) if(mask&(UINT32_C(1)<<w)) run.delta[w]=w+1;
    EffectBodyFunction function={.instruction_count=2,.runs=&run,.run_count=1,.run_capacity=1};
    EffectBodyMaterial material={.resources=context.resources,.references=1,.functions=&function};
    material.domain=(XirMetadataDomain){.types=&material.terms.types,.function_count=1};
    EffectBodyRequest request=effect_body_owned_request(&context,&material);
    EffectBodyRecordCursor borrowed,copied,before_tail={0};
    CHECK(effect_body_request_sequence_begin(&request,&borrowed)==XR_XIR_OK &&
        effect_body_request_sequence_begin(&request,&copied)==XR_XIR_OK);
    XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_F64,.immediate=INT64_MIN};
    XrXirInstruction expected_tail={0};uint64_t required=0;
    for(uint32_t i=0;i<2;++i) {
        uint32_t selected=i?mask:0;
        XrXirInstruction expected={.op=(XrXirOp)(XR_XIR_CONST_INT+((selected&1)?1:0)),
            .type=17+((selected&2)?2:0),.args={31+((selected&4)?3:0),41+((selected&8)?4:0)},
            .targets={51+((selected&16)?5:0),61+((selected&32)?6:0)},
            .type_arguments={81+((selected&256)?9:0),91+((selected&512)?10:0)}};
        immediate=(UINT64_C(0x12345678)+((selected&64)?7:0))|
            ((UINT64_C(0x80000001)+((selected&128)?8:0))<<32);
        memcpy(&expected.immediate,&immediate,sizeof(immediate));
        XrXirInstruction storage=sentinel,copy=sentinel;const XrXirInstruction *op=NULL,*old=NULL;
        uint64_t work=rp_stats(&context).work;if(i)before_tail=borrowed;
        CHECK(effect_body_request_sequence_operation(&request,0,i,&borrowed,&storage,&op)==XR_XIR_OK &&
            op==(selected?&storage:&run.base) && !memcmp(op,&expected,sizeof(expected)));
        uint64_t used=rp_stats(&context).work-work;work=rp_stats(&context).work;
        CHECK(effect_body_request_sequence_instruction(&request,0,i,&copied,&copy,&old)==XR_XIR_OK &&
            old==&copy && !memcmp(old,&expected,sizeof(expected)));
        if(selected) CHECK(used==rp_stats(&context).work-work);
        else CHECK(used<rp_stats(&context).work-work && !memcmp(&storage,&sentinel,sizeof(storage)));
        if(i){required=used;expected_tail=expected;}
    }
    CHECK(effect_body_request_sequence_finish(&request,0,&borrowed)==XR_XIR_OK &&
        effect_body_request_sequence_finish(&request,0,&copied)==XR_XIR_OK);
    XrXirInstruction storage=sentinel;const XrXirInstruction *op=(const XrXirInstruction *)&function;
    EffectBodyRecordCursor held=borrowed;
    CHECK(effect_body_request_sequence_operation(&request,0,2,&borrowed,&storage,&op)==XR_XIR_BAD_STRUCTURE &&
        op==(const XrXirInstruction *)&function && !memcmp(&storage,&sentinel,sizeof(storage)) &&
        !memcmp(&borrowed,&held,sizeof(held)));
    uint64_t work=rp_stats(&context).work;
    CHECK(required && work+required-less<rp_caps().work && xr_compile_resources_work(context.resources,
        rp_caps().work-work-required+less)==XR_COMPILE_RESOURCE_OK);
    borrowed=before_tail;storage=sentinel;op=(const XrXirInstruction *)&function;
    CHECK(effect_body_request_sequence_operation(&request,0,1,&borrowed,&storage,&op)==
        (less?XR_XIR_BUDGET:XR_XIR_OK));
    if(less) CHECK(op==(const XrXirInstruction *)&function);
    else CHECK(!memcmp(op,&expected_tail,sizeof(expected_tail)));
    if(!mask) CHECK(!memcmp(&storage,&sentinel,sizeof(storage)));
    CHECK(rp_attempts==attempts && rp_stats(&context).live_bytes==baseline);
    rp_owner_free(&context,baseline);rp_balanced(physical);CHECK(!rp_live && !rp_live_bytes);
}

static void sequence_borrow_binding(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;
    EffectBodyRun run={.count=2,.base={.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42}};
    EffectBodyFunction function={.instruction_count=2,.runs=&run,.run_count=1,.run_capacity=1};
    EffectBodyMaterial material={.resources=context.resources,.references=1,.functions=&function};
    material.domain=(XirMetadataDomain){.types=&material.terms.types,.function_count=1};
    EffectBodyRequest request=effect_body_owned_request(&context,&material);
    for(uint32_t damage=0;damage<15;++damage) {
        EffectBodyRequest changed=request;EffectBodyFunction saved_function=function;EffectBodyRun saved=run;
        EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
        if(!damage) changed.domain.types=&effect_empty_types;
        else if(damage==1)++changed.domain.function_count;
        else if(damage==2)changed.domain.declarations=(const XrXirDeclarations *)&material;
        else if(damage==3)changed.domain.generics=(const XrXirGeneric *)&material;
        else if(damage==4)changed.domain.defaults=(const XrXirDefaultTable *)&material;
        else if(damage==5)run.first=1;
        else if(damage==6)run.count=0;
        else if(damage==7)run.count=3;
        else if(damage==8)run.mask=1024;
        else if(damage==9)function.runs=NULL;
        else if(damage==10)function.run_capacity=0;
        else if(damage==11)run.base.op=XR_XIR_INVALID;
        else if(damage==12)run.base.op=XR_XIR_OP_COUNT;
        else if(damage==13)material.resources=NULL;
        else cursor.last=0;
        XrXirInstruction sentinel={.op=XR_XIR_RETURN,.immediate=INT64_MIN},storage=sentinel;
        const XrXirInstruction *op=(const XrXirInstruction *)&function;
        CHECK(effect_body_request_sequence_operation(&changed,0,0,&cursor,&storage,&op)==XR_XIR_BAD_STRUCTURE &&
            op==(const XrXirInstruction *)&function && !memcmp(&storage,&sentinel,sizeof(storage)));
        function=saved_function;run=saved;material.resources=context.resources;cursor=(EffectBodyRecordCursor){0,0,UINT32_MAX};
        CHECK(effect_body_request_sequence_operation(&request,0,0,&cursor,&storage,&op)==XR_XIR_OK &&
            op==&run.base && op->immediate==42);
    }
    EffectBodyRequest delegated=request;delegated.instruction=sequence_borrow_delegate;
    EffectBodyRecordCursor cursor={99,99,99},held=cursor;XrXirInstruction storage={0};
    const XrXirInstruction *op=NULL;uint64_t before=rp_stats(&context).work;sequence_borrow_delegate_calls=0;
    CHECK(effect_body_request_sequence_operation(&delegated,0,1,&cursor,&storage,&op)==XR_XIR_OK &&
        op==&storage && op->immediate==42 && sequence_borrow_delegate_calls==1 && !memcmp(&cursor,&held,sizeof(held)));
    uint64_t work=rp_stats(&context).work-before;before=rp_stats(&context).work;
    CHECK(effect_body_request_sequence_instruction(&delegated,0,1,&cursor,&storage,&op)==XR_XIR_OK &&
        sequence_borrow_delegate_calls==2 && rp_stats(&context).work-before==work);
    XrXirFunction raw_function={.instructions=&run.base,.instruction_count=1};
    XrXirModule module={.functions=&raw_function,.function_count=1};
    EffectBodyRequest raw=effect_body_raw_request(&context,&module);before=rp_stats(&context).work;
    CHECK(effect_body_request_sequence_operation(&raw,0,0,&cursor,&storage,&op)==XR_XIR_OK &&
        op==&run.base && rp_stats(&context).work-before==2+sizeof(op) && !memcmp(&cursor,&held,sizeof(held)));
    CHECK(rp_attempts==attempts);rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void sequence_borrow_cases(void) {
    for(uint32_t mask=0;mask<1024;++mask)for(uint32_t less=0;less<2;++less)sequence_borrow_mask(mask,less);
    sequence_borrow_binding();
    puts("SEQUENCE_BORROW all1024 literal complete rows,copy identity,exact/minus1,current binding/cursor/EOF,delegated/raw fees,physical0 PASS");
}
#endif // XIR_SEQUENCE_BORROW_CASES_H
