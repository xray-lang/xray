/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_timed_execution_cases.h - Real deadlines and typed Source outcomes
 */
#ifndef XIR_CHANNEL_TIMED_EXECUTION_CASES_H
#define XIR_CHANNEL_TIMED_EXECUTION_CASES_H
#include "xir/xxir_program.h"
#include "os/os_time.h"
#include "execution/xr_xir_host_execution.h"
typedef struct TimedSourceOutput { unsigned groups,reads; } TimedSourceOutput;
static XrXirTimeStatus timed_source_clock(void *context,XrXirClockKind clock,int64_t *nanoseconds) {
    TimedSourceOutput *w=context;CHECK(clock==XR_XIR_CLOCK_MONOTONIC);++w->reads;
    uint64_t reading=xr_time_monotonic_ns();if (!reading || reading>INT64_MAX) return XR_XIR_TIME_FAILED;
    *nanoseconds=(int64_t)reading;return XR_XIR_TIME_OK;
}
static XrXirTimeStatus timed_source_offset(void *context,int64_t seconds,int64_t *minutes) {
    (void)context;(void)seconds;(void)minutes;CHECK(false);return XR_XIR_TIME_FAILED;
}
static XrXirOutputStatus timed_source_output(void *context,const XrXirOutputGroup *group) {
    TimedSourceOutput *w=context;
    const int64_t expected[][3]={{1,51,-2},{-3,-3},{-2,-2},{-2,-2},{-2,15,-2},{-2},{-2,1},{1,7,2},{7}};
    const unsigned counts[]={3,2,2,2,3,1,2,3,1};
    CHECK(group && w->groups<9 && group->count==counts[w->groups]);
    for (unsigned i=0;i<group->count;++i) {
        if (group->values[i].type!=XR_XIR_I64 || (int64_t)group->values[i].payload!=expected[w->groups][i])
            fprintf(stderr,"Timed Source output group=%u item=%u type=%u value=%lld expected=%lld\n",w->groups,i,
                group->values[i].type,(long long)group->values[i].payload,(long long)expected[w->groups][i]);
        CHECK(group->values[i].type==XR_XIR_I64 && (int64_t)group->values[i].payload==expected[w->groups][i]);
    }
    ++w->groups;return XR_XIR_OUTPUT_OK;
}
static void timed_source_execute(XrXirProgram *program,uint32_t entry) {
    TimedSourceOutput witness={0};XrXirInstanceConfig config={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.time=(XrXirTimeProvider){XR_XIR_CALL_ABI_VERSION,0,timed_source_clock,timed_source_offset,&witness};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,timed_source_output,&witness};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    uint64_t started=xr_time_monotonic_ns();CHECK(started);XrXirInstanceResult result={0};unsigned pending=0;
    do {
        result=xr_xir_instance_poll_bounded(instance,64);
        if (result.outcome.status==XR_XIR_CALL_SUSPENDED) {
            XrXirWaitRequest wait={0};CHECK(xr_xir_instance_wait_request(instance,result.epoch,result.outcome.wake,&wait)==XR_XIR_CALL_READY);
            CHECK(wait.kind==XR_XIR_WAIT_CHANNEL);++pending;xr_time_sleep_ns(1000000);
        }
    } while (result.outcome.status==XR_XIR_CALL_READY || result.outcome.status==XR_XIR_CALL_SUSPENDED);
    if (result.outcome.status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"Timed Source status=%u groups=%u reads=%u\n",result.outcome.status,witness.groups,witness.reads);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && witness.groups==9 && witness.reads && pending);
    CHECK(xr_time_monotonic_ns()-started>=UINT64_C(30000000));
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==99);
    xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
    printf("Timed Source nine typed output groups, Unit evaluated once, child provider, real deadlines; reads=%u pending=%u physical0\n",witness.reads,pending);
}
static void timed_source_host_execute(XrXirProgram *program,uint32_t entry) {
    TimedSourceOutput witness={0};XrXirInstanceConfig config={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.time=(XrXirTimeProvider){XR_XIR_CALL_ABI_VERSION,0,timed_source_clock,timed_source_offset,&witness};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,timed_source_output,&witness};
    XrXirHostExecutionRequest request={program,&config,entry,NULL,0};XrXirCallResult result={0};
    uint64_t started=xr_time_monotonic_ns();CHECK(started);
    CHECK(xr_xir_host_execute(&request,&result)==XR_XIR_CALL_RETURNED && result.status==XR_XIR_CALL_RETURNED);
    CHECK(result.value.type==XR_XIR_I64 && result.value.payload==99 && witness.groups==9 && witness.reads);
    CHECK(xr_time_monotonic_ns()-started>=UINT64_C(30000000));
    xr_xir_call_result_drop(&result);xr_xir_compile_program_drop(program);CHECK(!runtime_live && !runtime_bytes);
    puts("Timed actual generic host: nine typed groups and real monotonic Channel waits physical0 PASS");
}

#endif // XIR_CHANNEL_TIMED_EXECUTION_CASES_H
