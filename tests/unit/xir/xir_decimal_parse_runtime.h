/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_decimal_parse_runtime.h - Exact runtime expectations and complete allocation faults
 */
#ifndef XIR_DECIMAL_PARSE_RUNTIME_H
#define XIR_DECIMAL_PARSE_RUNTIME_H
static XrXirValue decimal_execute(XrXirProgram *program,uint32_t entry) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,65536).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return value;
}
static void decimal_runtime_attempt(XrXirProgram *program,uint32_t entry,bool injected) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;XrXirValue output={0};bool entered_oom=false;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if(status==XR_XIR_CALL_OOM)CHECK(!instance);
    if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,entry,NULL,0);
    if(status==XR_XIR_CALL_READY){status=xr_xir_instance_poll_bounded(instance,65536).outcome.status;entered_oom=status==XR_XIR_CALL_OOM;}
    if(status==XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(instance,&output)==XR_XIR_CALL_RETURNED);
        CHECK(output.type==XR_XIR_I64 && !output.reserved && output.payload==42);xr_xir_value_drop(&output);
    } else if(instance) {
        CHECK(xr_xir_instance_take_result(instance,&output)==(entered_oom?XR_XIR_CALL_OOM:XR_XIR_CALL_BAD_STATE));
        CHECK(!output.type && !output.reserved && !output.payload);
    }
    if(instance)CHECK(xr_xir_instance_free(instance)==(entered_oom?XR_XIR_CALL_OOM:XR_XIR_CALL_READY));
    CHECK(status==(injected?XR_XIR_CALL_OOM:XR_XIR_CALL_RETURNED));
}
static XrXirValue decimal_program_cases(XrXirProgram *program,const uint32_t functions[3]) {
    size_t live=runtime_live,bytes=runtime_bytes,compiler_live=source_fixture_compile_live,compiler_bytes=source_fixture_compile_bytes;
    runtime_attempts=0;decimal_runtime_attempt(program,functions[0],false);size_t sites=runtime_attempts;CHECK(sites);
    for(size_t at=0;at<sites;++at) {
        runtime_attempts=0;runtime_fail_at=at;decimal_runtime_attempt(program,functions[0],true);runtime_fail_at=SIZE_MAX;
        CHECK(runtime_attempts>at && runtime_live==live && runtime_bytes==bytes &&
            source_fixture_compile_live==compiler_live && source_fixture_compile_bytes==compiler_bytes);
        decimal_runtime_attempt(program,functions[0],false);
        CHECK(runtime_live==live && runtime_bytes==bytes && source_fixture_compile_live==compiler_live && source_fixture_compile_bytes==compiler_bytes);
    }
    fprintf(stderr,"Decimal runtime all %zu allocation sites: sticky entered OOM/Unit, exact free, same Program retry, both physical baselines\n",sites);
    XrXirValue kept=decimal_execute(program,functions[1]);xr_xir_compile_program_drop(program);return kept;
}
static void decimal_retained(XrXirValue *kept) {
    bool some=false;const XrXirValue *payload=NULL;
    CHECK(xr_xir_nullable_view(kept,&some,&payload) && some && payload && payload->type==XR_XIR_F64 &&
        !payload->reserved && (uint64_t)payload->payload==UINT64_C(0x7fefffffffffffff));
    xr_xir_value_drop(kept);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
}
#endif // XIR_DECIMAL_PARSE_RUNTIME_H
