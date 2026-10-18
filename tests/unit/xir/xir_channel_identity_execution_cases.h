/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_identity_execution_cases.h - Independent typed identity operation oracles
 */
#ifndef XIR_CHANNEL_IDENTITY_EXECUTION_CASES_H
#define XIR_CHANNEL_IDENTITY_EXECUTION_CASES_H
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
typedef struct ChannelIdentityOutput { char bytes[128];size_t length;unsigned groups;XrXirOutputSink sink; } ChannelIdentityOutput;
static XrXirOutputStatus channel_identity_bytes(void *context,XrXirOutputStream stream,const char *bytes,size_t size) {
    ChannelIdentityOutput *output=context;CHECK(stream==XR_XIR_STDOUT && size<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,size);output->length+=size;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus channel_identity_group(void *context,const XrXirOutputGroup *group) {
    ChannelIdentityOutput *output=context;CHECK(group && output->groups<2);
    const uint32_t first[]={XR_XIR_I64,XR_XIR_BOOL,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64};
    CHECK(group->count==(output->groups ? 3u : 5u));
    for (uint32_t i=0;i<group->count;++i) CHECK(group->values[i].type==(output->groups ? XR_XIR_BOOL : first[i]));
    ++output->groups;return xr_xir_output_render(&output->sink,group);
}
static void channel_identity_run(XrXirInstance *instance,uint32_t entry,ChannelIdentityOutput *output,bool failure) {
    output->length=0;output->groups=0;CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult poll={0};unsigned polls=0;
    do { CHECK(++polls<4096);poll=xr_xir_instance_poll_bounded(instance,1); } while(poll.outcome.status==XR_XIR_CALL_READY);
    if (poll.outcome.status!=(failure ? XR_XIR_CALL_NUMERIC_RANGE : XR_XIR_CALL_RETURNED))
        fprintf(stderr,"Channel identity status=%u failure=%u polls=%u\n",poll.outcome.status,failure,polls);
    CHECK(xr_xir_call_result_valid(&poll.outcome));
    CHECK(poll.outcome.status==(failure ? XR_XIR_CALL_NUMERIC_RANGE : XR_XIR_CALL_RETURNED));
    if (failure) { CHECK(!output->length && !output->groups); return; }
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && !result.reserved && result.payload==3);xr_xir_value_drop(&result);
    const char expected[]="3 false 0 1 2\ntrue true false\n";
    CHECK(output->groups==2 && output->length==sizeof(expected)-1 && !memcmp(output->bytes,expected,sizeof(expected)-1));
}
static void channel_identity_execute(XrXirProgram *program,const uint32_t entries[3]) {
    ChannelIdentityOutput outputs[2]={{0},{0}};XrXirInstance *instances[2]={NULL,NULL};
    for (unsigned i=0;i<2;++i) {
        outputs[i].sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,channel_identity_bytes,&outputs[i],128};
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,channel_identity_group,&outputs[i]};
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    channel_identity_run(instances[0],entries[0],&outputs[0],false);
    channel_identity_run(instances[1],entries[0],&outputs[1],false);
    channel_identity_run(instances[0],entries[0],&outputs[0],false);
    channel_identity_run(instances[0],entries[1],&outputs[0],true);
    channel_identity_run(instances[1],entries[2],&outputs[1],true);
    CHECK(xr_xir_instance_free(instances[1])==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(instances[0])==XR_XIR_CALL_READY);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Channel identity two-instance normal/range cleanup physical=0/0");
}
static size_t channel_identity_fault_once(XrXirProgram *program,uint32_t entry,size_t ordinal) {
    size_t live=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;
    ChannelIdentityOutput output={0};output.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,channel_identity_bytes,&output,128};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,channel_identity_group,&output};
    runtime_fail_at=ordinal==SIZE_MAX ? SIZE_MAX : begin+ordinal;
    XrXirInstance *instance=NULL;XrXirCallStatus release_expected=XR_XIR_CALL_READY;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if (status==XR_XIR_CALL_READY) {
        status=xr_xir_instance_start(instance,entry,NULL,0);
        if (status==XR_XIR_CALL_READY) {
            unsigned polls=0;XrXirInstanceResult result={0};
            do { CHECK(++polls<4096);result=xr_xir_instance_poll_bounded(instance,1); } while(result.outcome.status==XR_XIR_CALL_READY);
            status=result.outcome.status;CHECK(xr_xir_call_result_valid(&result.outcome));
            if (status==XR_XIR_CALL_RETURNED) {
                XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==status);
                CHECK(value.type==XR_XIR_I64 && value.payload==3);xr_xir_value_drop(&value);
            } else release_expected=status;
        }
    }
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    CHECK(status==(ordinal==SIZE_MAX ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_OOM));
    const char expected[]="3 false 0 1 2\ntrue true false\n";
    CHECK(output.length<=sizeof(expected)-1 && !memcmp(output.bytes,expected,output.length));
    if (ordinal==SIZE_MAX) CHECK(output.length==sizeof(expected)-1 && output.groups==2);
    size_t attempts=runtime_attempts;CHECK(xr_xir_instance_free(instance)==release_expected && runtime_attempts==attempts);
    CHECK(runtime_live==live && runtime_bytes==bytes);return sites;
}
static void channel_identity_runtime_faults(XrXirProgram *program,uint32_t entry) {
    size_t sites=channel_identity_fault_once(program,entry,SIZE_MAX);CHECK(sites && sites<4096);
    for (size_t ordinal=0;ordinal<sites;++ordinal) {
        CHECK(channel_identity_fault_once(program,entry,ordinal)>ordinal);
        CHECK(channel_identity_fault_once(program,entry,SIZE_MAX)==sites);
    }
    xr_xir_compile_program_drop(program);CHECK(!runtime_live && !runtime_bytes);
    printf("Channel identity runtime complete allocator ordinals=%zu physical=0 same-process recovery=PASS\n",sites);
}
#endif // XIR_CHANNEL_IDENTITY_EXECUTION_CASES_H
