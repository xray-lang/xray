/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_delta_group_cases.h - All physical masks and exact work boundaries
 */
#ifndef XIR_DELTA_GROUP_CASES_H
#define XIR_DELTA_GROUP_CASES_H

static void delta_mask_case(uint32_t mask,uint32_t less,bool full) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    EffectBodyRead read=effect_body_compile_read(&context);
    EffectBodyRun run={.first=0,.count=2,.mask=mask,.base={.op=XR_XIR_CONST_INT,.type=XR_XIR_I64}};
    for(uint32_t w=0;w<EFFECT_BODY_WORDS;++w)run.delta[w]=w+1;
    EffectBodyFunction function={.instruction_count=2,.runs=&run,.run_count=1,.run_capacity=1};
    const EffectBodyRun *old=NULL,*fresh=NULL;XrXirInstruction row={0};size_t attempts=rp_attempts;
    CHECK(delta_original_run_ref(&read,&function,0,0,&old)==XR_XIR_OK && old==&run);
    CHECK(effect_body_run_ref(&read,&function,0,0,&fresh)==XR_XIR_OK && fresh==old);
    CHECK(delta_original_instruction_ref_prepare(&read,&run,1)==XR_XIR_OK);
    CHECK(effect_body_instruction_ref_value(&read,&run,1,&row)==XR_XIR_OK);
    for(uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
        uint32_t expected=effect_body_source_word(&run.base,w)+((mask&(UINT32_C(1)<<w))?run.delta[w]:0);
        CHECK(effect_body_source_word(&row,w)==expected);
    }
    for(uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
        if(!(mask&(UINT32_C(1)<<w)))continue;
        uint32_t saved=run.delta[w];run.delta[w]=0;
        old=fresh=(const EffectBodyRun *)&function;
        CHECK(delta_original_run_ref(&read,&function,0,0,&old)==XR_XIR_BAD_STRUCTURE && old==(const EffectBodyRun *)&function);
        CHECK(effect_body_run_ref(&read,&function,0,0,&fresh)==XR_XIR_BAD_STRUCTURE && fresh==old);
        CHECK(delta_original_instruction_ref_prepare(&read,&run,1)==XR_XIR_BAD_STRUCTURE);
        XrXirInstruction before={.op=XR_XIR_RETURN,.type=XR_XIR_F64,.immediate=INT64_MIN};row=before;
        CHECK(effect_body_instruction_ref_value(&read,&run,1,&row)==XR_XIR_BAD_STRUCTURE && !memcmp(&row,&before,sizeof(row)));
        run.delta[w]=saved;
    }
    uint32_t saved_mask=run.mask;run.mask|=1024;
    CHECK(effect_body_run_ref(&read,&function,0,0,&fresh)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_body_instruction_ref_prepare(&read,&run,1)==XR_XIR_BAD_STRUCTURE);run.mask=saved_mask;
    uint32_t saved_count=run.count;run.count=1;
    if(mask)CHECK(effect_body_run_ref(&read,&function,0,0,&fresh)==XR_XIR_BAD_STRUCTURE);
    run.count=saved_count;
    XrCompileResourceStats before=rp_stats(&context);
    if(full)CHECK(effect_body_instruction_ref_value(&read,&run,1,&row)==XR_XIR_OK);
    else CHECK(effect_body_run_ref(&read,&function,0,0,&fresh)==XR_XIR_OK);
    XrCompileResourceStats after=rp_stats(&context);CHECK(rp_attempts==attempts && after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes);
    uint64_t required=after.work-before.work,limit=rp_caps().work,remaining=required-less;
    CHECK(required && after.work+remaining<limit && xr_compile_resources_work(context.resources,limit-after.work-remaining)==XR_COMPILE_RESOURCE_OK);
    XrXirStatus status;
    if(full) {
        XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_F64,.immediate=INT64_MIN};row=sentinel;
        status=effect_body_instruction_ref_value(&read,&run,1,&row);
        CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK));
        if(less)CHECK(!memcmp(&row,&sentinel,sizeof(row)));
    } else {
        fresh=(const EffectBodyRun *)&function;status=effect_body_run_ref(&read,&function,0,0,&fresh);
        CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK) && fresh==(less?(const EffectBodyRun *)&function:&run));
    }
    rp_owner_free(&context,baseline);rp_balanced(physical);CHECK(!rp_live && !rp_live_bytes);
}
#include "xir_single_run_decode_cases.h"
#include "xir_borrowed_instruction_cases.h"
#include "xir_invocation_fixed_data_cases.h"
#include "xir_runtime_current_key_cases.h"
#include "xir_runtime_capture_walk_cases.h"

static void delta_group_cases(void) {
    single_run_decode_cases();borrowed_instruction_cases();fixed_data_cases();runtime_current_key_cases();runtime_capture_walk_cases();
    runtime_origin_identity_cases();
    for(uint32_t mask=0;mask<1024;++mask)for(uint32_t less=0;less<2;++less)for(uint32_t full=0;full<2;++full)
        delta_mask_case(mask,less,full!=0);
    puts("All1024 physical masks,all selected zero slots,original scalar equality,complete row/exact-minus1,unchanged failure outputs,no allocations,physical0 PASS");
}

#endif // XIR_DELTA_GROUP_CASES_H
