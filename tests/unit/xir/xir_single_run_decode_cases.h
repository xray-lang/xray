/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_single_run_decode_cases.h - Complete sole-run words and failure boundaries
 */
#ifndef XIR_SINGLE_RUN_DECODE_CASES_H
#define XIR_SINGLE_RUN_DECODE_CASES_H

typedef struct SingleRunRead {
    const XrXirCompileContext *work;uint32_t calls,reject;
    EffectBodyFunction *target;EffectBodyRun *replacement;uint32_t change,mode;
} SingleRunRead;
static bool single_run_consume(const void *owner,uint64_t units) {
    SingleRunRead *read=(SingleRunRead *)owner;
    uint32_t call=read->calls++;
    if (call==read->reject) return false;
    if (read->target && call==read->change) {
        if (!read->mode) read->target->runs=read->replacement;
        else if (read->mode==1) ++read->target->run_count;
        else if (read->mode==2) ++read->target->instruction_count;
        else read->target->run_capacity=0;
    }
    return xir_compile_work(read->work,units);
}

/* Literal physical words exercise storage, not semantic instruction admission.
 * Unselected poisoned delta slots must never contribute to a decoded field. */
static void single_run_mask_case(uint32_t mask,uint32_t less) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;
    EffectBodyRun run;memset(&run,0xCE,sizeof(run));
    run.first=0;run.count=2;run.mask=mask;run.base=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    for (uint32_t w=0;w<10;++w) if (mask&(UINT32_C(1)<<w)) run.delta[w]=w+1;
    EffectBodyFunction function={.instruction_count=2,.runs=&run,.run_count=1,.run_capacity=1};
    XrXirInstruction expected={.op=(XrXirOp)(XR_XIR_CONST_INT+((mask&1)?1:0)),
        .type=XR_XIR_I64+((mask&2)?2:0),.args={(mask&4)?3:0,(mask&8)?4:0},
        .targets={(mask&16)?5:0,(mask&32)?6:0},.type_arguments={(mask&256)?9:0,(mask&512)?10:0}};
    uint64_t immediate=((mask&64)?UINT64_C(7):0)|(((mask&128)?UINT64_C(8):0)<<32);
    memcpy(&expected.immediate,&immediate,sizeof(immediate));
    XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_F64,.immediate=INT64_MIN},row=sentinel;
    SingleRunRead probe={.work=&context,.reject=UINT32_MAX};EffectBodyRead read={&probe,single_run_consume};
    XrCompileResourceStats before=rp_stats(&context);
    CHECK(effect_body_instruction_get(&read,&function,1,&row)==XR_XIR_OK && !memcmp(&row,&expected,sizeof(row)));
    uint64_t required=rp_stats(&context).work-before.work;uint32_t calls=probe.calls;
    CHECK(calls && required && rp_attempts==attempts);
    for (uint32_t reject=0;reject<calls;++reject) {
        probe.calls=0;probe.reject=reject;row=sentinel;
        CHECK(effect_body_instruction_get(&read,&function,1,&row)==XR_XIR_BUDGET &&
            !memcmp(&row,&sentinel,sizeof(row)) && probe.calls==reject+1);
    }
    probe.reject=UINT32_MAX;
    for (uint32_t w=0;w<10;++w) if (mask&(UINT32_C(1)<<w)) {
        uint32_t saved=run.delta[w];run.delta[w]=0;row=sentinel;
        CHECK(effect_body_instruction_get(&read,&function,1,&row)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&row,&sentinel,sizeof(row)));run.delta[w]=saved;
    }
    for (uint32_t damage=0;damage<8;++damage) {
        EffectBodyFunction saved_function=function;EffectBodyRun saved=run;row=sentinel;
        if (!damage) run.first=1;
        else if (damage==1) run.count=3;
        else if (damage==2) run.count=0;
        else if (damage==3) run.mask|=1024;
        else if (damage==4) function.runs=NULL;
        else if (damage==5) function.run_capacity=0;
        else if (damage==6) function.instruction_count=0;
        else run.base.op=XR_XIR_OP_COUNT;
        CHECK(effect_body_instruction_get(&read,&function,1,&row)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&row,&sentinel,sizeof(row)));function=saved_function;run=saved;
    }
    row=sentinel;
    CHECK(effect_body_instruction_get(&read,&function,2,&row)==XR_XIR_BAD_STRUCTURE && !memcmp(&row,&sentinel,sizeof(row)));
    XrCompileResourceStats after=rp_stats(&context);uint64_t maximum=rp_caps().work;
    CHECK(after.work+required-less<maximum &&
        xr_compile_resources_work(context.resources,maximum-after.work-required+less)==XR_COMPILE_RESOURCE_OK);
    row=sentinel;
    CHECK(effect_body_instruction_get(&read,&function,1,&row)==(less?XR_XIR_BUDGET:XR_XIR_OK));
    CHECK(!memcmp(&row,less?&sentinel:&expected,sizeof(row)) && rp_attempts==attempts &&
        rp_stats(&context).allocated_bytes==before.allocated_bytes && rp_stats(&context).live_bytes==before.live_bytes);
    rp_owner_free(&context,baseline);rp_balanced(physical);CHECK(!rp_live && !rp_live_bytes);
}
/* Only literal storage is changed by the last consume callback. A directory
 * reference cannot escape after its current array, dimensions or capacity moved. */
static void directory_current_binding_cases(void) {
    for (uint32_t count=1;count<=5;++count) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;
        EffectBodyRun runs[5]={0},replacement[5];
        for (uint32_t r=0;r<count;++r) runs[r]=(EffectBodyRun){.first=r*2,.count=2,
            .base={.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=(int64_t)(r*r)}};
        memcpy(replacement,runs,sizeof(runs));
        EffectBodyFunction function={.instruction_count=count*2,.runs=runs,.run_count=count,.run_capacity=count};
        uint32_t ordinals[3]={0,count,count*2-1};
        for (uint32_t which=0;which<3;++which) {
            SingleRunRead probe={.work=&context,.reject=UINT32_MAX};EffectBodyRead read={&probe,single_run_consume};
            const EffectBodyRun *held=(const EffectBodyRun *)&function,*output=held;
            CHECK(effect_body_record_ref(&read,&function,ordinals[which],&output)==XR_XIR_OK &&
                output==&runs[ordinals[which]/2] && probe.calls);
            uint32_t calls=probe.calls;
            for (uint32_t mode=0;mode<4;++mode) {
                EffectBodyFunction saved=function;output=held;
                probe=(SingleRunRead){.work=&context,.reject=UINT32_MAX,.target=&function,
                    .replacement=replacement,.change=calls-1,.mode=mode};
                CHECK(effect_body_record_ref(&read,&function,ordinals[which],&output)==XR_XIR_BAD_STRUCTURE &&
                    output==held && probe.calls==calls);function=saved;
            }
            probe=(SingleRunRead){.work=&context,.reject=UINT32_MAX};output=held;
            CHECK(effect_body_record_ref(&read,&function,ordinals[which],&output)==XR_XIR_OK &&
                output==&runs[ordinals[which]/2]);
        }
        CHECK(rp_attempts==attempts);rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    puts("Directory runs1..5, first/interior/tail, 60 current-binding mutations, restored positives, unchanged outputs, no allocations, physical0 PASS");
}

static void single_run_decode_cases(void) {
    directory_current_binding_cases();
    for (uint32_t mask=0;mask<1024;++mask) for (uint32_t less=0;less<2;++less) single_run_mask_case(mask,less);
    puts("Single run all1024 masks, literal ten-word rows, poisoned inactive slots, selected-zero/header refusals, every read refusal, exact/minus1, unchanged output, no allocations, physical0 PASS");
}
#endif // XIR_SINGLE_RUN_DECODE_CASES_H
