/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derivation_json_runtime.h - Actual JSON string result ownership
 */
#ifndef XIR_SOURCE_DERIVATION_JSON_RUNTIME_H
#define XIR_SOURCE_DERIVATION_JSON_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
static void json_runtime_expected(const XrXirValue *value,bool text) {
    if (!text) {CHECK(value->type==XR_XIR_I64 && !value->reserved && value->payload==42);return;}
    const char expected[]="{\"left\":42,\"secret\":-7,\"enabled\":true,\"label\":\"\\\"\\\\\\u000a\\u0009\\u0000汉😀\"}";
    const char *bytes=NULL;size_t length=0;
    CHECK(value->type==XR_XIR_STRING && xr_xir_string_view(value,&bytes,&length) &&
        length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
}
static XrXirValue json_runtime_take(XrXirProgram *program,uint32_t function) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return value;
}
static void json_runtime_attempt(XrXirProgram *program,uint32_t entry,bool text) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;XrXirValue value={0};bool driven=false;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if (status==XR_XIR_CALL_OOM) CHECK(!instance);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,entry,NULL,0);
    if (status==XR_XIR_CALL_READY) {status=xr_xir_instance_poll_bounded(instance,4096).outcome.status;driven=status==XR_XIR_CALL_OOM;}
    CHECK(status==XR_XIR_CALL_RETURNED || (runtime_fail_at!=SIZE_MAX && status==XR_XIR_CALL_OOM));
    if (status==XR_XIR_CALL_RETURNED) CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    else if (instance) {
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE);
        CHECK(!value.type && !value.reserved && !value.payload);
    }
    if (instance) CHECK(xr_xir_instance_free(instance)==(driven?XR_XIR_CALL_OOM:XR_XIR_CALL_READY));
    if (status==XR_XIR_CALL_RETURNED) {json_runtime_expected(&value,text);xr_xir_value_drop(&value);}
}
static size_t json_runtime_failures(XrXirProgram *program,uint32_t entry,bool text,bool faults) {
    size_t held=runtime_live,bytes=runtime_bytes;runtime_attempts=0;
    json_runtime_attempt(program,entry,text);size_t sites=runtime_attempts;
    CHECK(sites && runtime_live==held && runtime_bytes==bytes);
    if (faults) for (size_t site=0;site<sites;++site) {
        runtime_attempts=0;runtime_fail_at=site;json_runtime_attempt(program,entry,text);
        CHECK(runtime_attempts>site && runtime_live==held && runtime_bytes==bytes);
        runtime_fail_at=SIZE_MAX;json_runtime_attempt(program,entry,text);
        CHECK(runtime_live==held && runtime_bytes==bytes);
    }
    return sites;
}
#endif // XIR_SOURCE_DERIVATION_JSON_RUNTIME_H
