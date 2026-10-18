/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_worker_cases.h - Actual worker producer authority before body execution
 */
#ifndef XIR_GENERATOR_WORKER_CASES_H
#define XIR_GENERATOR_WORKER_CASES_H
#include "xir/xxir_generator_internal.h"
typedef struct WorkerBoundaryLog {unsigned cleanup;} WorkerBoundaryLog;
static XrXirOutputStatus worker_boundary_output(void *owner,const XrXirOutputGroup *group) {
    WorkerBoundaryLog *log=owner;
    CHECK(group && group->line && group->stream==XR_XIR_STDOUT && group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64 && group->values[0].payload==401);++log->cleanup;
    return XR_XIR_OUTPUT_OK;
}
static XrXirInstance *worker_boundary_instance(XrXirProgram *program,WorkerBoundaryLog *log) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,worker_boundary_output,log};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);return instance;
}
static XrXirValue worker_boundary_return(XrXirInstance *instance) {
    CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void worker_boundary_positive(XrXirProgram *program,const uint32_t ids[11],bool task) {
    WorkerBoundaryLog log={0};XrXirInstance *instance=worker_boundary_instance(program,&log);
    CHECK(xr_xir_instance_start(instance,ids[task ? 6 : 0],NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult pending=xr_xir_instance_poll_bounded(instance,1000000);XrXirWaitRequest wait={0};
    CHECK(pending.outcome.status==XR_XIR_CALL_SUSPENDED && pending.epoch && pending.outcome.wake);
    CHECK(xr_xir_instance_wait_request(instance,pending.epoch,pending.outcome.wake,&wait)==XR_XIR_CALL_READY);
    CHECK(wait.kind==XR_XIR_WAIT_TIMER_MS && wait.after_ms==5 && !log.cleanup);
    CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_READY);
    XrXirValue value=worker_boundary_return(instance);CHECK(value.type==XR_XIR_I64 && value.payload==(task ? 1 : 7));
    xr_xir_value_drop(&value);CHECK(log.cleanup==1);
    CHECK(xr_xir_instance_start(instance,ids[1],NULL,0)==XR_XIR_CALL_READY);
    value=worker_boundary_return(instance);CHECK(value.type==XR_XIR_I64 && value.payload==(task ? 0 : 1));
    xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.cleanup==1);
}
static void worker_boundary_reject(XrXirProgram *program,const uint32_t ids[11],unsigned family,bool replace) {
    WorkerBoundaryLog log={0};XrXirInstance *instance=worker_boundary_instance(program,&log);
    CHECK(xr_xir_instance_start(instance,ids[family<4 ? family+2 : family+4],NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    if(result.outcome.status!=XR_XIR_CALL_BAD_ARGUMENT || log.cleanup)
        fprintf(stderr,"Worker boundary family%u replace%u status%u cleanup%u\n",family,replace ? 1u : 0u,result.outcome.status,log.cleanup);
    CHECK(result.outcome.status==XR_XIR_CALL_BAD_ARGUMENT && !result.outcome.wake && xr_xir_call_result_valid(&result.outcome));
    CHECK(!log.cleanup && !result.outcome.value.type);
    if(replace) {
        CHECK(xr_xir_instance_start(instance,ids[1],NULL,0)==XR_XIR_CALL_READY);
        XrXirValue value=worker_boundary_return(instance);CHECK(value.type==XR_XIR_I64 && !value.payload);xr_xir_value_drop(&value);
    }
    XrXirCallStatus freed=xr_xir_instance_free(instance);
    if(freed!=(replace ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_ARGUMENT)) fprintf(stderr,"Boundary free family%u replace%u status%u\n",family,replace ? 1u : 0u,freed);
    CHECK(freed==(replace ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_ARGUMENT) && !log.cleanup);
}
static void worker_boundary_park(XrXirProgram *program,const uint32_t ids[11]) {
    WorkerBoundaryLog log={0};XrXirInstance *instance=worker_boundary_instance(program,&log);
    CHECK(xr_xir_instance_start(instance,ids[7],NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    if(result.outcome.status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"Cold producer expected return11, observed%u\n",result.outcome.status);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && !result.outcome.wake && !log.cleanup);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && value.payload==11);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_start(instance,ids[1],NULL,0)==XR_XIR_CALL_READY);
    value=worker_boundary_return(instance);CHECK(value.type==XR_XIR_I64 && !value.payload);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && !log.cleanup);
}
static void worker_boundary_cases(XrXirProgram *program,const uint32_t ids[11]) {
    worker_boundary_park(program,ids);
    worker_boundary_positive(program,ids,false);worker_boundary_positive(program,ids,true);
    for(unsigned family=0;family<7;++family) for(unsigned replace=0;replace<2;++replace)
        worker_boundary_reject(program,ids,family,replace!=0);
    puts("Generator worker boundary:3 legal controls +14 exact runtime refusals before body; static consumer-origin precision OPEN");
}
#endif
