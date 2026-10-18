/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_blocking_execution_cases.h - Independent typed delivery oracle
 */
#ifndef XIR_CHANNEL_BLOCKING_EXECUTION_CASES_H
#define XIR_CHANNEL_BLOCKING_EXECUTION_CASES_H
#include "xir/xxir_program.h"
typedef struct BlockingOutput { unsigned groups; } BlockingOutput;
static XrXirOutputStatus blocking_output(void *context,const XrXirOutputGroup *group) {
    BlockingOutput *output=context;
    const int64_t expected[][4]={{42,7,-3,-5},{113,114},{211},{61,61},{3,31,-3},{317,318},{62,63,4},{6,6},{41,7,7},{17,95},{451},{64}};
    const unsigned counts[]={4,2,1,2,3,2,3,2,3,2,1,1};
    CHECK(group && output->groups<12 && group->count==counts[output->groups]);
    for (unsigned i=0;i<group->count;++i)
        CHECK(group->values[i].type==XR_XIR_I64 && group->values[i].payload==expected[output->groups][i]);
    ++output->groups;
    return XR_XIR_OUTPUT_OK;
}
static void blocking_execute(XrXirProgram *program,uint32_t entry) {
    BlockingOutput output={0};XrXirInstanceConfig config={0};CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,blocking_output,&output};XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);unsigned polls=0;XrXirInstanceResult result={0};
    do { CHECK(++polls<4096);result=xr_xir_instance_poll_bounded(instance,1); } while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && output.groups==12);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==99);
    xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
}
static size_t blocking_fault_once(XrXirProgram *program,uint32_t entry,size_t ordinal) {
    size_t live=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;BlockingOutput output={0};XrXirInstanceConfig config={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,blocking_output,&output};
    runtime_fail_at=ordinal==SIZE_MAX ? SIZE_MAX : begin+ordinal;
    XrXirInstance *instance=NULL;XrXirCallStatus release=XR_XIR_CALL_READY,status=xr_xir_instance_new(program,&config,&instance);
    if (status==XR_XIR_CALL_READY) {
        status=xr_xir_instance_start(instance,entry,NULL,0);
        if (status==XR_XIR_CALL_READY) {
            unsigned polls=0;XrXirInstanceResult result={0};
            do { CHECK(++polls<4096);result=xr_xir_instance_poll_bounded(instance,1); } while(result.outcome.status==XR_XIR_CALL_READY);
            status=result.outcome.status;CHECK(xr_xir_call_result_valid(&result.outcome));
            if (status==XR_XIR_CALL_RETURNED) {
                XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==status && value.type==XR_XIR_I64 && value.payload==99);
                xr_xir_value_drop(&value);
            } else release=status;
        }
    }
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (status!=(ordinal==SIZE_MAX ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_OOM))
        fprintf(stderr,"Blocking Source runtime ordinal=%zu sites=%zu status=%u output_groups=%u\n",ordinal,sites,status,output.groups);
    CHECK(status==(ordinal==SIZE_MAX ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_OOM));
    if (ordinal==SIZE_MAX) CHECK(output.groups==12);
    size_t attempts=runtime_attempts;CHECK(xr_xir_instance_free(instance)==release && runtime_attempts==attempts);
    CHECK(runtime_live==live && runtime_bytes==bytes);return sites;
}
static void blocking_runtime_faults(XrXirProgram *program,uint32_t entry) {
    size_t sites=blocking_fault_once(program,entry,SIZE_MAX);CHECK(sites && sites<4096);
    for (size_t ordinal=0;ordinal<sites;++ordinal) {
        CHECK(blocking_fault_once(program,entry,ordinal)>ordinal);
        CHECK(blocking_fault_once(program,entry,SIZE_MAX)==sites);
    }
    xr_xir_compile_program_drop(program);CHECK(!runtime_live && !runtime_bytes);
    printf("Blocking actual Source runtime complete allocator ordinals=%zu; independent typed output prefix and same-process recovery physical0\n",sites);
}
#endif // XIR_CHANNEL_BLOCKING_EXECUTION_CASES_H
