/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_text_rune_runtime.h - Fixed scalar, panic, cleanup and owned-NUL results
 *
 * KEY CONCEPT:
 *   The same source entries execute with independent literals in each backend.
 */
#ifndef XIR_TEXT_RUNE_RUNTIME_H
#define XIR_TEXT_RUNE_RUNTIME_H
static XrXirInstanceResult text_drive(XrXirInstance *instance) {
    XrXirInstanceResult result;
    do {result=xr_xir_instance_poll_bounded(instance,16);}while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(!result.outcome.wake);return result;
}
static XrXirValue text_return(XrXirInstance *instance,uint32_t entry,const XrXirValue *args,uint32_t count) {
    CHECK(xr_xir_instance_start(instance,entry,args,count)==XR_XIR_CALL_READY);
    XrXirInstanceResult outcome=text_drive(instance);XrXirValue result={0};
    CHECK(outcome.outcome.status==XR_XIR_CALL_RETURNED && xr_xir_panic_empty(&outcome.outcome.panic));
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);return result;
}
static XrXirValue text_rune_runtime(XrXirInstance *instance,const uint32_t *ids) {
    static const int64_t good[]={0,0x41,0xd7ff,0xe000,0x1f600,0x10ffff};
    static const int64_t bad[]={-1,0xd800,0xdbff,0xdc00,0xdfff,0x110000};
    for(size_t i=0;i<sizeof(good)/sizeof(good[0]);++i) {
        XrXirValue input={XR_XIR_I64,0,good[i]},rune=text_return(instance,ids[1],&input,1);
        CHECK(rune.type==XR_XIR_RUNE && !rune.reserved && rune.payload==good[i]);
        XrXirValue point=text_return(instance,ids[2],&rune,1);
        CHECK(point.type==XR_XIR_I64 && !point.reserved && point.payload==good[i]);
        xr_xir_value_drop(&point);xr_xir_value_drop(&rune);
    }
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        XrXirValue input={XR_XIR_I64,0,bad[i]};
        CHECK(xr_xir_instance_start(instance,ids[1],&input,1)==XR_XIR_CALL_READY);
        XrXirInstanceResult result=text_drive(instance);
        CHECK(result.outcome.status==XR_XIR_CALL_NUMERIC_RANGE && result.outcome.panic.detail.code==422);
        CHECK(xr_xir_call_result_valid(&result.outcome) && result.outcome.value.type==XR_XIR_UNIT);
        CHECK(xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY);
    }
    XrXirValue forged={XR_XIR_RUNE,0,INT64_C(0x100000041)};
    size_t before=runtime_attempts;
    CHECK(xr_xir_instance_start(instance,ids[2],&forged,1)==XR_XIR_CALL_BAD_ARGUMENT && runtime_attempts==before);
    XrXirValue value=text_return(instance,ids[4],NULL,0);
    CHECK(value.type==XR_XIR_I64 && value.payload==9);xr_xir_value_drop(&value);
    value=text_return(instance,ids[5],NULL,0);
    CHECK(value.type==XR_XIR_I64 && value.payload==76);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_start(instance,ids[6],NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=text_drive(instance);
    CHECK(result.outcome.status==XR_XIR_CALL_NUMERIC_RANGE && result.outcome.panic.detail.code==422);
    value=text_return(instance,ids[3],NULL,0);text_value(&value,"\0",1);return value;
}
static size_t text_nul_attempt(XrXirProgram *program,uint32_t entry,size_t fail) {
    XrXirInstanceConfig config;TextOutputState unused={0};text_config(&config,&unused);config.output=(XrXirOutputProvider){0};
    XrXirInstance *instance=NULL;XrXirValue value={0};
    runtime_attempts=0;runtime_fail_at=fail;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if(status==XR_XIR_CALL_READY) {
        status=xr_xir_instance_start(instance,entry,NULL,0);
        if(status==XR_XIR_CALL_READY)status=text_drive(instance).outcome.status;
        if(status==XR_XIR_CALL_RETURNED) {
            CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);text_value(&value,"\0",1);
        }
    }
    size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
    if(fail==SIZE_MAX)CHECK(status==XR_XIR_CALL_RETURNED);
    else CHECK(status==XR_XIR_CALL_OOM && attempts>fail);
    if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    if(value.type)text_value(&value,"\0",1);
    xr_xir_value_drop(&value);
    CHECK(!runtime_live && !runtime_bytes);return attempts;
}
static void text_nul_faults(XrXirProgram *program,uint32_t entry) {
    size_t points=text_nul_attempt(program,entry,SIZE_MAX);CHECK(points);
    for(size_t i=0;i<points;++i)(void)text_nul_attempt(program,entry,i);
    fprintf(stderr,"owned Rune NUL: %zu real runtime allocator failures; physical=0/0\n",points);
}
#endif // XIR_TEXT_RUNE_RUNTIME_H
