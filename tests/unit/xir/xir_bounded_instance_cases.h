/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_bounded_instance_cases.h - Cancel/drain/restart on real VM and native programs
 */
#ifndef XIR_BOUNDED_INSTANCE_CASES_H
#define XIR_BOUNDED_INSTANCE_CASES_H
static XrXirInstanceResult instance_until_pause(XrXirInstance *instance) {
    XrXirInstanceResult result={0}; unsigned steps=0;
    do { result=xr_xir_instance_poll_bounded(instance,1); CHECK(++steps<10000); }
    while (result.outcome.status==XR_XIR_CALL_READY);
    return result;
}
typedef struct BoundedReentry { XrXirInstance *instance; unsigned writes, observations; } BoundedReentry;
static XrXirOutputStatus bounded_reentry_output(void *context,const XrXirOutputGroup *group) {
    BoundedReentry *w=context;
    CHECK(group->count==1 && group->values[0].type==XR_XIR_I64 && group->values[0].payload==1);
    CHECK(xr_xir_instance_cancel_current(w->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_poll_bounded(w->instance,1).outcome.status==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_free(w->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_stop(w->instance)==XR_XIR_CALL_READY);
    ++w->writes; return XR_XIR_OUTPUT_OK;
}
static void bounded_reentry_trace(void *context,XrXirLifecycleEvent event,uint32_t index) {
    BoundedReentry *w=context; (void)event; (void)index;
    CHECK(xr_xir_instance_cancel_current(w->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_poll_bounded(w->instance,1).outcome.status==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_stop(w->instance)==XR_XIR_CALL_BUSY);
    ++w->observations;
}
static void bounded_instances(XrXirProgram *program,uint32_t normal,uint32_t current,uint32_t spin,uint32_t reader) {
    size_t live=runtime_live,bytes=runtime_bytes;
    HostTrace trace={0}; XrXirInstanceConfig config=host_config(&trace); XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_start(instance,normal,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_state(instance)==XR_XIR_INSTANCE_INITIALIZING);
    CHECK(xr_xir_instance_poll_bounded(instance,0).outcome.status==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED && !trace.count);
    XrXirInstanceResult result=instance_until_pause(instance);
    CHECK(result.outcome.status==XR_XIR_CALL_CANCELLED && xr_xir_instance_state(instance)==XR_XIR_INSTANCE_FAILED);
    CHECK(xr_xir_instance_start(instance,normal,NULL,0)==XR_XIR_CALL_CANCELLED);
    XrXirCallResult failure={0};
    CHECK(xr_xir_instance_copy_failure(instance,&failure)==XR_XIR_CALL_CANCELLED);
    xr_xir_call_result_drop(&failure);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==live && runtime_bytes==bytes); instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,normal,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult suspended=instance_until_pause(instance);
    CHECK(suspended.outcome.status==XR_XIR_CALL_SUSPENDED && trace.count==1 && trace.values[0]==1);
    CHECK(xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY);
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED && trace.count==1);
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED && trace.count==1);
    CHECK(xr_xir_instance_start(instance,current,NULL,0)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_resume(instance,suspended.epoch,suspended.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    result=instance_until_pause(instance);
    CHECK(result.outcome.status==XR_XIR_CALL_CANCELLED && result.epoch==suspended.epoch);
    CHECK(trace.count==2 && trace.values[1]==1 && xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY);
    CHECK(xr_xir_instance_start(instance,current,NULL,0)==XR_XIR_CALL_READY);
    result=instance_until_pause(instance);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && result.outcome.value.payload==1 && result.epoch==suspended.epoch+1);
    CHECK(xr_xir_instance_start(instance,reader,NULL,0)==XR_XIR_CALL_READY);
    CHECK(instance_until_pause(instance).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue function={0}; CHECK(xr_xir_instance_take_result(instance,&function)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_start(instance,spin,NULL,0)==XR_XIR_CALL_READY);
    for (unsigned i=0;i<4;++i) CHECK(xr_xir_instance_poll_bounded(instance,8).outcome.status==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(instance_until_pause(instance).outcome.status==XR_XIR_CALL_CANCELLED);
    CHECK(xr_xir_instance_start_function(instance,&function,NULL,0)==XR_XIR_CALL_READY);
    result=instance_until_pause(instance); CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && result.outcome.value.payload==1);
    xr_xir_value_drop(&function);
    CHECK(xr_xir_instance_start(instance,normal,NULL,0)==XR_XIR_CALL_READY);
    result=instance_until_pause(instance);
    CHECK(result.outcome.status==XR_XIR_CALL_SUSPENDED && result.epoch>suspended.epoch && trace.count==3 && trace.values[2]==2);
    CHECK(xr_xir_instance_resume(instance,suspended.epoch,suspended.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(instance_until_pause(instance).outcome.status==XR_XIR_CALL_CANCELLED && trace.count==4 && trace.values[3]==2);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    XrXirHostCall *host=NULL; trace=(HostTrace){0}; config=host_config(&trace);
    XrXirHostExecutionRequest request={program,&config,normal,NULL,0};
    CHECK(xr_xir_host_call_begin(&request,&host)==XR_XIR_CALL_READY);
    result=xr_xir_host_call_step_bounded(host,1); CHECK(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(xr_xir_host_call_step_bounded(host,0).outcome.status==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_host_call_request_cancel(host)==XR_XIR_CALL_CANCEL_REQUESTED && !trace.count);
    XrXirCallResult owned={0}; CHECK(xr_xir_host_call_take(host,&owned)==XR_XIR_CALL_BAD_STATE);
    do { result=xr_xir_host_call_step_bounded(host,1); } while (result.outcome.status==XR_XIR_CALL_READY);
    CHECK(result.outcome.status==XR_XIR_CALL_CANCELLED);
    CHECK(xr_xir_host_call_take(host,&owned)==XR_XIR_CALL_CANCELLED);
    CHECK(xr_xir_host_call_drop(host)==XR_XIR_CALL_READY); xr_xir_call_result_drop(&owned);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    request.entry=spin;
    CHECK(xr_xir_host_execute(&request,&owned)==XR_XIR_CALL_RETURNED);
    CHECK(owned.value.type==XR_XIR_I64 && owned.value.payload==1000);
    xr_xir_call_result_drop(&owned); CHECK(runtime_live==live && runtime_bytes==bytes);
    BoundedReentry reentry={0}; CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,bounded_reentry_output,&reentry};
    config.trace=bounded_reentry_trace; config.trace_context=&reentry;
    CHECK(xr_xir_instance_new(program,&config,&reentry.instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(reentry.instance,normal,NULL,0)==XR_XIR_CALL_READY);
    CHECK(instance_until_pause(reentry.instance).outcome.status==XR_XIR_CALL_CANCELLED);
    CHECK(reentry.writes==2 && reentry.observations);
    CHECK(xr_xir_instance_free(reentry.instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    puts("real program bounded CPU slices, initialization cancel, restart/module state and epoch PASS");
}
#endif
