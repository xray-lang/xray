/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_state_failure_cases.h - Checked initialization failures and owned sticky outcomes
 *
 * KEY CONCEPT:
 *   Whole Programs preserve failure identity and release real owners.
 */

#ifndef XIR_MODULE_STATE_FAILURE_CASES_H
#define XIR_MODULE_STATE_FAILURE_CASES_H
#include "xir/xxir_panic.h"
typedef struct StateFailureTrace {
    XrXirInstance *instance; uint32_t begins[4],ready[4],published,released,busy,output;
} StateFailureTrace;
static void state_failure_trace(void *context,XrXirLifecycleEvent event,uint32_t index) {
    StateFailureTrace *trace=context;CHECK(trace && trace->instance);
    CHECK(xr_xir_instance_free(trace->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_start(trace->instance,4,NULL,0)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_cancel_current(trace->instance)==XR_XIR_CALL_BUSY);++trace->busy;
    if(event==XR_XIR_MODULE_BEGIN) {CHECK(index==0 && !trace->begins[index]);++trace->begins[index];}
    else if(event==XR_XIR_MODULE_READY) {CHECK(index<4);++trace->ready[index];}
    else if(event==XR_XIR_SLOT_PUBLISHED) {CHECK(index==0 && !trace->published);++trace->published;}
    else {CHECK(event==XR_XIR_SLOT_RELEASED && index==0 && trace->published && !trace->released);++trace->released;}
}
static XrXirOutputStatus state_failure_output(void *context,const XrXirOutputGroup *group) {
    StateFailureTrace *trace=context;CHECK(trace && group && group->stream==XR_XIR_STDOUT);++trace->output;
    return XR_XIR_OUTPUT_OK;
}
static void state_failure_outcome(const XrXirCallResult *outcome,uint32_t scenario) {
    CHECK(outcome->status==(scenario==4?XR_XIR_CALL_ASSERTION:XR_XIR_CALL_BAD_STATE));
    CHECK(!outcome->wake && !outcome->value.type && !outcome->value.reserved && !outcome->value.payload);
    if(scenario==4) {
        CHECK(outcome->panic.detail.code==XR_XIR_PANIC_ASSERTION && !outcome->panic.detail.reserved &&
              !outcome->panic.detail.index && !outcome->panic.detail.length);
        const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&outcome->panic.message,&bytes,&length));
        CHECK(length==28 && !memcmp(bytes,"module initializer assertion",28));
    } else CHECK(xr_xir_panic_empty(&outcome->panic));
}
static void state_failure_pair(XrXirProgram *program,uint32_t scenario) {
    CHECK(program && !runtime_live && !runtime_bytes);
    size_t compiler_blocks=source_program_compile_live,compiler_bytes=source_program_compile_bytes;
    StateFailureTrace traces[2]={0};XrXirInstance *instances[2]={0};
    XrXirCallResult retained[2]={{0},{0}};uint64_t epochs[2]={0};uintptr_t messages[2]={0};
    for(uint32_t i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.metadata_limit=1024*1024;config.value_limit=1024*1024;config.call_limit=1024*1024;
        config.poll_limit=1000000;config.depth_limit=64;config.trace=state_failure_trace;config.trace_context=&traces[i];
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,state_failure_output,&traces[i]};
        for(uint32_t version=4;version<=6;++version) {
            XrXirInstanceConfig stale=config;stale.abi_version=version;XrXirInstance *rejected=NULL;
            CHECK(xr_xir_instance_new(program,&stale,&rejected)==XR_XIR_CALL_BAD_ABI && !rejected);
        }
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
        traces[i].instance=instances[i];
    }
    xr_xir_compile_program_drop(program);program=NULL;
    CHECK(source_program_compile_live==compiler_blocks && source_program_compile_bytes==compiler_bytes);
    for(uint32_t repeat=0;repeat<24;++repeat)for(uint32_t i=0;i<2;++i) {
        size_t attempts=runtime_attempts;
        XrXirCallStatus started=xr_xir_instance_start(instances[i],4,NULL,0);
        CHECK(started==(repeat?(scenario==4?XR_XIR_CALL_ASSERTION:XR_XIR_CALL_BAD_STATE):XR_XIR_CALL_READY));
        XrXirInstanceResult result;
        do {result=xr_xir_instance_poll_bounded(instances[i],16);}while(result.outcome.status==XR_XIR_CALL_READY);
        state_failure_outcome(&result.outcome,scenario);
        CHECK(xr_xir_instance_state(instances[i])==XR_XIR_INSTANCE_FAILED);
        if(!repeat) {epochs[i]=result.epoch;messages[i]=(uintptr_t)result.outcome.panic.message.payload;}
        else CHECK(result.epoch==epochs[i] && (uintptr_t)result.outcome.panic.message.payload==messages[i] && runtime_attempts==attempts);
        XrXirValue sentinel={XR_XIR_I64,0,91},output=sentinel;
        CHECK(xr_xir_instance_take_result(instances[i],&output)==XR_XIR_CALL_BAD_STATE);
        CHECK(!memcmp(&output,&sentinel,sizeof(output)));
        CHECK(traces[i].begins[0]==1 && !traces[i].output);
        for(uint32_t m=0;m<4;++m)CHECK(!traces[i].ready[m] && (m==0 || !traces[i].begins[m]));
        CHECK(traces[i].published==(scenario==2?0u:1u) && traces[i].released==traces[i].published);
        CHECK(!instances[i]->publication_count && !instances[i]->published[0]);
    }
    for(uint32_t i=0;i<2;++i) {
        CHECK(xr_xir_instance_copy_failure(instances[i],&retained[i])==(scenario==4?XR_XIR_CALL_ASSERTION:XR_XIR_CALL_BAD_STATE));
        state_failure_outcome(&retained[i],scenario);
        XrXirCallResult occupied={.status=XR_XIR_CALL_RETURNED,.value={XR_XIR_I64,0,91}},before=occupied;
        CHECK(xr_xir_instance_copy_failure(instances[i],&occupied)==XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&occupied,&before,sizeof(before)));
        CHECK(xr_xir_instance_stop(instances[i])==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);traces[i].instance=NULL;
        CHECK(traces[i].busy==(scenario==2?1u:3u));
    }
    if(scenario==4)CHECK(messages[0]!=messages[1] && runtime_live && runtime_bytes);
    for(uint32_t i=0;i<2;++i) {state_failure_outcome(&retained[i],scenario);xr_xir_call_result_drop(&retained[i]);}
    CHECK(!runtime_live && !runtime_bytes);
    printf("state %u: two instances x24 sticky status, no entry/output, publication cleanup, escaped failure and physical0\n",scenario);
}
#endif
