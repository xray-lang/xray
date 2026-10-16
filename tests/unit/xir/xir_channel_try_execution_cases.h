/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_try_execution_cases.h - Independent message and logical-copy oracles
 */
#ifndef XIR_CHANNEL_TRY_EXECUTION_CASES_H
#define XIR_CHANNEL_TRY_EXECUTION_CASES_H
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
typedef struct ChannelTryOutput { char bytes[256];size_t length;unsigned groups;XrXirOutputSink sink; } ChannelTryOutput;
static XrXirOutputStatus channel_try_bytes(void *context,XrXirOutputStream stream,const char *bytes,size_t size) {
    ChannelTryOutput *output=context;CHECK(stream==XR_XIR_STDOUT && size<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,size);output->length+=size;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus channel_try_group(void *context,const XrXirOutputGroup *group) {
    ChannelTryOutput *output=context;const unsigned counts[]={4,2,3,3,4,3,2,3};
    CHECK(group && output->groups<8 && group->count==counts[output->groups]);
    for (uint32_t i=0;i<group->count;++i) CHECK(group->values[i].type==
        (uint32_t)(output->groups==4 && i==2 ? XR_XIR_STRING : output->groups==7 && i==2 ? XR_XIR_BOOL : XR_XIR_I64));
    ++output->groups;return xr_xir_output_render(&output->sink,group);
}
static void channel_try_run(XrXirInstance *instance,uint32_t entry,ChannelTryOutput *output) {
    output->length=0;output->groups=0;CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result={0};unsigned polls=0;
    do { CHECK(++polls<16384);result=xr_xir_instance_poll_bounded(instance,1); } while(result.outcome.status==XR_XIR_CALL_READY);
    if (result.outcome.status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"Channel try status=%u polls=%u\n",result.outcome.status,polls);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && xr_xir_call_result_valid(&result.outcome));
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==99);xr_xir_value_drop(&value);
    const char expected[]="-1 1 2 42\n-3 4\n1 11 99\n1 99 1\n1 1 nested 1\n2 -1 0\n99 2\n1 123 true\n";
    if (output->length!=sizeof(expected)-1 || memcmp(output->bytes,expected,sizeof(expected)-1))
        fprintf(stderr,"Channel try actual output=%.*s\n",(int)output->length,output->bytes);
    CHECK(output->groups==8 && output->length==sizeof(expected)-1 && !memcmp(output->bytes,expected,sizeof(expected)-1));
}
static void channel_try_execute(XrXirProgram *program,uint32_t entry) {
    ChannelTryOutput output[2]={{0},{0}};XrXirInstance *instances[2]={NULL,NULL};
    for (unsigned i=0;i<2;++i) {
        output[i].sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,channel_try_bytes,&output[i],256};
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,channel_try_group,&output[i]};
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);channel_try_run(instances[0],entry,&output[0]);
    channel_try_run(instances[1],entry,&output[1]);channel_try_run(instances[0],entry,&output[0]);
    CHECK(xr_xir_instance_free(instances[1])==XR_XIR_CALL_READY && xr_xir_instance_free(instances[0])==XR_XIR_CALL_READY);
    CHECK(!runtime_live && !runtime_bytes);puts("Channel try independent typed output, logical Array copy, Unit and nested identity physical=0/0");
}
static size_t channel_try_fault_once(XrXirProgram *program,uint32_t entry,size_t ordinal) {
    size_t live=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;
    ChannelTryOutput output={0};output.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,channel_try_bytes,&output,256};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,channel_try_group,&output};
    runtime_fail_at=ordinal==SIZE_MAX ? SIZE_MAX : begin+ordinal;
    XrXirInstance *instance=NULL;XrXirCallStatus release_expected=XR_XIR_CALL_READY;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if (status==XR_XIR_CALL_READY) {
        status=xr_xir_instance_start(instance,entry,NULL,0);
        if (status==XR_XIR_CALL_READY) {
            unsigned polls=0;XrXirInstanceResult result={0};
            do { CHECK(++polls<16384);result=xr_xir_instance_poll_bounded(instance,1); } while(result.outcome.status==XR_XIR_CALL_READY);
            status=result.outcome.status;CHECK(xr_xir_call_result_valid(&result.outcome));
            if (status==XR_XIR_CALL_RETURNED) {
                XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==status);
                CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==99);xr_xir_value_drop(&value);
            } else release_expected=status;
        }
    }
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (status!=(ordinal==SIZE_MAX ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_OOM))
        fprintf(stderr,"Channel try runtime fault ordinal=%zu sites=%zu status=%u\n",ordinal,sites,status);
    CHECK(status==(ordinal==SIZE_MAX ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_OOM));
    const char expected[]="-1 1 2 42\n-3 4\n1 11 99\n1 99 1\n1 1 nested 1\n2 -1 0\n99 2\n1 123 true\n";
    CHECK(output.length<=sizeof(expected)-1 && !memcmp(output.bytes,expected,output.length));
    if (ordinal==SIZE_MAX) CHECK(output.length==sizeof(expected)-1 && output.groups==8);
    size_t attempts=runtime_attempts;CHECK(xr_xir_instance_free(instance)==release_expected && runtime_attempts==attempts);
    CHECK(runtime_live==live && runtime_bytes==bytes);return sites;
}
static void channel_try_runtime_faults(XrXirProgram *program,uint32_t entry) {
    size_t sites=channel_try_fault_once(program,entry,SIZE_MAX);CHECK(sites && sites<4096);
    for (size_t ordinal=0;ordinal<sites;++ordinal) {
        CHECK(channel_try_fault_once(program,entry,ordinal)>ordinal);
        CHECK(channel_try_fault_once(program,entry,SIZE_MAX)==sites);
    }
    xr_xir_compile_program_drop(program);CHECK(!runtime_live && !runtime_bytes);
    printf("Channel try runtime complete allocator ordinals=%zu physical=0 same-process recovery=PASS\n",sites);
}
#endif // XIR_CHANNEL_TRY_EXECUTION_CASES_H
