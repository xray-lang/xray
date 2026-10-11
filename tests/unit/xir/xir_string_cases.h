/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_string_cases.h - Independent string execution and physical release expectations
 *
 * KEY CONCEPT:
 *   Real Instances preserve independent outcomes and actual retirement ledgers.
 */
#ifndef XIR_STRING_CASES_H
#define XIR_STRING_CASES_H
#include "xir_error_fixture.h"
#include "xir_source_fixture_owner.h"
#include "xir_string_instance_probe.h"
static const char string_left[]="A\0\xE4\xB8\xAD";
static const char string_right[]="\xF0\x9F\x98\x80!";
static const char string_expected[]="A\0\xE4\xB8\xAD\xF0\x9F\x98\x80!";
typedef struct StringOutput { uint32_t calls,mode; XrXirInstance *instance; } StringOutput;
static void string_bytes(const XrXirValue *value,const char *expected,size_t size) {
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length));
    CHECK(length==size && !memcmp(bytes,expected,size));
}
static XrXirOutputStatus string_output(void *context,const XrXirOutputGroup *group) {
    CHECK(group && !group->line && group->count==1);
    StringOutput *output=context;
    string_bytes(&group->values[0],string_expected,sizeof(string_expected)-1);
    CHECK(group->stream==(output->mode==5 || output->mode==6 || output->calls ? XR_XIR_STDOUT:XR_XIR_STDERR));
    ++output->calls;
    xr_string_instance_output_busy(output->instance,output->mode==6 && output->calls==3);
    return output->mode!=3?XR_XIR_OUTPUT_OK:XR_XIR_OUTPUT_ERROR;
}
static void string_error_bytes(const XrXirValue *value,XrXirDomain *domain) {
    XrXirValueAdmission admission={xr_xir_value_arena(value),domain,NULL,NULL,10000,65536};
    XrXirValue field={0};CHECK(xr_xir_enum_get(value,1,0,&admission,&field)==XR_XIR_VALUE_OK);
    string_bytes(&field,string_expected,sizeof(string_expected)-1);xr_xir_value_drop(&field);
}
static XrXirValue string_cases(XrXirProgram *program,uint32_t variant,uint32_t mode) {
    StringOutput output={0,mode,NULL};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=65536;config.value_limit=mode==8?240:65536;config.call_limit=65536;
    config.poll_limit=mode==5?15:100;config.depth_limit=10;
    config.output=mode==4?(XrXirOutputProvider){0}:
        (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,string_output,&output};
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY && instance);
    output.instance=instance;
    XrXirDomain *domain=xr_string_instance_domain(instance),*input_domain=NULL;
    if (mode==8 || mode==9) CHECK(xr_xir_domain_new(65536,&input_domain)==XR_XIR_VALUE_OK);
    XrXirDomain *arguments_domain=input_domain?input_domain:domain;
    XrXirValue arguments[2]={{0},{0}},owned={0};
    CHECK(xr_xir_string_new(arguments_domain,string_left,sizeof(string_left)-1,&arguments[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(arguments_domain,string_right,sizeof(string_right)-1,&arguments[1])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_instance_start(instance,mode==5 || mode==6?2:0,arguments,2)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&arguments[0]);xr_xir_value_drop(&arguments[1]);
    CHECK(xr_xir_instance_take_result(instance,&owned)==XR_XIR_CALL_BAD_STATE && !owned.type);
    XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    XrXirCallResult result=polled.outcome;
    if (mode==3 || mode==4) CHECK(result.status==XR_XIR_CALL_OUTPUT_ERROR);
    else if (mode==5 || mode==8) CHECK(result.status==XR_XIR_CALL_LIMIT);
    else if (mode==6) CHECK(result.status==XR_XIR_CALL_CANCELLED && output.calls==3);
    else {
        CHECK(result.status==XR_XIR_CALL_SUSPENDED && output.calls==1);
        CHECK(xr_xir_instance_take_result(instance,&owned)==XR_XIR_CALL_BAD_STATE && !owned.type);
        if (mode==1) {
            CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
            CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_CANCELLED);
        } else if (mode!=2) {
            CHECK(xr_xir_instance_resume(instance,polled.epoch,result.wake)==XR_XIR_CALL_READY);
            result=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome;
            if (variant) { CHECK(result.status==XR_XIR_CALL_THROWN);string_error_bytes(&result.value,domain); }
            else {
                CHECK(result.status==XR_XIR_CALL_RETURNED && output.calls==2);
                string_bytes(&result.value,string_expected,sizeof(string_expected)-1);
            }
            if (mode!=7) {
                CHECK(xr_xir_instance_take_result(instance,&owned)==result.status);
                CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_CONSUMED);
                XrXirValue second={0};
                CHECK(xr_xir_instance_take_result(instance,&second)==XR_XIR_CALL_BAD_STATE && !second.type);
            }
        }
    }
    const XrXirCallStatus expected=mode==3 || mode==4?XR_XIR_CALL_OUTPUT_ERROR:
        mode==5 || mode==8?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY;
    if (mode!=2) CHECK(xr_string_instance_completion(instance)==expected);
    XirStringRetirement retired=xr_string_instance_retire(instance,expected,config.poll_limit);
    CHECK(retired.domain==domain);
    if (!owned.type) {
        XrXirDomainStats stats=xr_xir_domain_stats(domain);
        CHECK(stats.live_bytes==retired.baseline.live_bytes && stats.allocations==stats.frees+1);
    } else {
        if (owned.type==XR_XIR_STRING) string_bytes(&owned,string_expected,sizeof(string_expected)-1);
        else {
            XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
            string_error_bytes(&owned,reader);xr_xir_domain_drop(reader);
        }
        CHECK(xr_xir_domain_stats(domain).live_bytes>retired.baseline.live_bytes);
    }
    xr_xir_domain_drop(input_domain);xr_xir_domain_drop(domain);
    return owned;
}
#endif // XIR_STRING_CASES_H
