/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_activation_cases.h - Actual activation reclamation and lifetime ownership
 */
#ifndef XIR_ACTIVATION_CASES_H
#define XIR_ACTIVATION_CASES_H
static void activation_close_without_allocation(XrXirInstance *instance) {
    size_t attempts=runtime_attempts;
    runtime_fail_at=attempts;
    CHECK(xr_xir_instance_close_activation(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_attempts==attempts);
    runtime_fail_at=SIZE_MAX;
}
static void activation_cleanup_failure(XrXirProgram *program,uint32_t cleanup) {
    size_t live=runtime_live,bytes=runtime_bytes;
    HostTrace trace={0}; XrXirInstanceConfig config=host_config(&trace);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,cleanup,NULL,0)==XR_XIR_CALL_READY);
    CHECK(instance_until_pause(instance).outcome.status==XR_XIR_CALL_SUSPENDED);
    runtime_fail_at=runtime_attempts;
    CHECK(xr_xir_instance_close_activation(instance)==XR_XIR_CALL_OOM);
    runtime_fail_at=SIZE_MAX;
    CHECK(!instance->call && !instance->executor && instance->stopping && !trace.count);
    CHECK(xr_xir_instance_start(instance,cleanup,NULL,0)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==live && runtime_bytes==bytes);
}
static void activation_cases(XrXirProgram *program,uint32_t normal,uint32_t current,
    uint32_t reader,uint32_t children) {
    size_t live=runtime_live,bytes=runtime_bytes;
    HostTrace trace={0}; XrXirInstanceConfig config=host_config(&trace);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_close_activation(NULL)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,normal,NULL,0)==XR_XIR_CALL_READY);
    activation_close_without_allocation(instance);
    CHECK(!trace.count && xr_xir_instance_state(instance)==XR_XIR_INSTANCE_FAILED);
    XrXirCallResult failure={0};
    CHECK(xr_xir_instance_copy_failure(instance,&failure)==XR_XIR_CALL_CANCELLED);
    CHECK(xr_xir_instance_start(instance,current,NULL,0)==XR_XIR_CALL_CANCELLED);
    xr_xir_call_result_drop(&failure);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,normal,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult paused=instance_until_pause(instance);
    CHECK(paused.outcome.status==XR_XIR_CALL_SUSPENDED && trace.count==1 && trace.values[0]==1);
    XrXirDomain *domain=instance->domain;
    activation_close_without_allocation(instance);
    CHECK(trace.count==2 && trace.values[1]==1 && xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY);
    CHECK(!instance->call && !instance->executor && instance->domain==domain);
    CHECK(xr_xir_instance_resume(instance,paused.epoch,paused.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_start(instance,reader,NULL,0)==XR_XIR_CALL_READY);
    CHECK(instance_until_pause(instance).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue function={0};
    CHECK(xr_xir_instance_take_result(instance,&function)==XR_XIR_CALL_RETURNED);
    activation_close_without_allocation(instance);
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(domain);
    runtime_attempts=0;
    CHECK(xr_xir_instance_start(instance,current,NULL,0)==XR_XIR_CALL_READY);
    size_t sites=runtime_attempts;
    CHECK(sites>0);
    XrXirInstanceResult result=instance_until_pause(instance);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && result.outcome.value.payload==1 && result.epoch>paused.epoch);
    activation_close_without_allocation(instance);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(domain);
    CHECK(after.work>before.work && after.work_limit==before.work_limit && instance->domain==domain);
    for (size_t point=0;point<sites;++point) {
        runtime_attempts=0; runtime_fail_at=point;
        CHECK(xr_xir_instance_start(instance,current,NULL,0)==XR_XIR_CALL_OOM);
        runtime_fail_at=SIZE_MAX;
        CHECK(!instance->call && !instance->executor && instance->domain==domain);
        CHECK(xr_xir_instance_start_function(instance,&function,NULL,0)==XR_XIR_CALL_READY);
        result=instance_until_pause(instance);
        CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && result.outcome.value.payload==1);
        activation_close_without_allocation(instance);
    }
    CHECK(xr_xir_instance_start(instance,children,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,128).outcome.status==XR_XIR_CALL_READY);
    activation_close_without_allocation(instance);
    CHECK(trace.count==4 && trace.values[2]+trace.values[3]==147 && trace.values[2]!=trace.values[3]);
    CHECK(xr_xir_instance_start_function(instance,&function,NULL,0)==XR_XIR_CALL_READY);
    result=instance_until_pause(instance);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && result.outcome.value.payload==1);
    activation_close_without_allocation(instance);
    CHECK(xr_xir_instance_close_activation(instance)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&function);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    printf("actual activation close, root/child cleanup, callable authority and %zu restart faults PASS\n",sites);
}
#endif // XIR_ACTIVATION_CASES_H
