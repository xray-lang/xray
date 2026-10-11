/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_runtime_retry.h - Actual fault epochs and same-owner rebind
 */
#ifndef XIR_CLASS_ACCESS_RUNTIME_RETRY_H
#define XIR_CLASS_ACCESS_RUNTIME_RETRY_H
static bool access_runtime_retry_blocked(AccessRuntimeRun *run,unsigned pass) {
    XrXirInstance *instance=run->instances[pass];
    bool stopping=instance->stopping;
    bool busy=!xr_xir_task_executor_root_idle(instance->executor);
    bool cleanup=instance->executor->cleanup_incomplete || xr_xir_call_cleanup_incomplete(instance->call);
    if (!stopping && !busy && !cleanup && !instance->budget.exhausted) return false;
    uint64_t epoch=instance->epoch;size_t blocks=runtime_live,bytes=runtime_bytes,sites=runtime_attempts;
    access_phase(ACCESS_RETRY,pass);
    XrXirCallStatus status=xr_xir_instance_start(instance,run->entries->run,NULL,0);
    CHECK(instance->epoch==epoch && runtime_live==blocks && runtime_bytes==bytes);
    if (stopping || busy) {
        CHECK(status==(stopping?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_BUSY) && runtime_attempts==sites);
    } else if (access_resource_status(status)) access_failure(run,pass,status);
    else CHECK(cleanup && status==XR_XIR_CALL_BAD_STATE);
    printf("CLASS_ACCESS_RUNTIME_BLOCKED fixture=%s mode=%u owner=%u stop=%u busy=%u cleanup=%u exhausted=%u status=%u sameEpoch=1 physicalHeld=1\n",
        run->fixture->name,run->mode,pass,(unsigned)stopping,(unsigned)busy,(unsigned)cleanup,
        (unsigned)instance->budget.exhausted,(unsigned)status);
    return true;
}
static void access_runtime_rebind(AccessRuntimeRun *run,unsigned pass) {
    XrXirInstance *instance=run->instances[pass];
    if (!instance || !run->failed[pass] || run->cost.failure!=XR_XIR_CALL_OOM) return;
    XrXirInstanceState state=xr_xir_instance_state(instance);
    if (instance->state==XR_XIR_INSTANCE_FAILED) return; /* The original failed epoch was retried and proved sticky. */
    if (access_runtime_retry_blocked(run,pass)) return;
    CHECK(state==XR_XIR_INSTANCE_READY || state==XR_XIR_INSTANCE_NEW);
    XrXirDomain *domain=instance->domain;uint64_t epoch=instance->epoch;
    XrXirInstanceConfig config=instance->config;
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(domain);
    XrXirValue result={0},counter={0};
    XrXirCallStatus expected=run->fixture->alias?XR_XIR_CALL_BAD_ARGUMENT:XR_XIR_CALL_RETURNED;
    bool complete=access_entry(run,pass,run->entries->run,&result,expected);
    if (complete && run->fixture->alias) {
        complete=access_entry(run,pass,run->entries->counter,&counter,XR_XIR_CALL_RETURNED);
        if (complete) access_number(&counter,0);
    } else if (complete) access_number(&result,run->fixture->result);
    CHECK(instance->domain==domain && instance->budget.work_domain==domain);
    CHECK(!memcmp(&config,&instance->config,sizeof(config)));
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(domain);
    CHECK(after.requested_bytes>=before.requested_bytes && after.requested_call_bytes>=before.requested_call_bytes && after.work>=before.work);
    if (complete) {CHECK(instance->epoch>epoch);++run->cost.rebinds[pass];}
    xr_xir_value_drop(&result);xr_xir_value_drop(&counter);
    printf("CLASS_ACCESS_RUNTIME_REBIND fixture=%s mode=%u owner=%u complete=%u epochBefore=%llu epochAfter=%llu sameDomain=1 noRefund=1\n",
        run->fixture->name,run->mode,pass,(unsigned)complete,(unsigned long long)epoch,(unsigned long long)instance->epoch);
}
#endif // XIR_CLASS_ACCESS_RUNTIME_RETRY_H
