/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_runtime_transactions.h - Genuine entry preparation and transfer
 */
#ifndef XIR_CLASS_ACCESS_RUNTIME_TRANSACTIONS_H
#define XIR_CLASS_ACCESS_RUNTIME_TRANSACTIONS_H
static AccessRuntimeRun *access_runtime_run;
static void access_phase(unsigned phase,unsigned pass) {
    cell_runtime_phase=phase;cell_runtime_instance=pass;cell_runtime_poll=0;
}
static void cell_runtime_event_role(CellRuntimeAllocationEvent *event) {
    AccessRuntimeRun *run=access_runtime_run;
    if (!run || event->instance>=2) return;
    if (event->injected) ++run->injected[event->instance];
    XrXirInstance *instance=run->instances[event->instance];
    if (!instance || !instance->executor) return;
    XrXirTaskExecutor *executor=instance->executor;
    XrXirCall *call=executor->current?executor->current->call:instance->call;
    if (!call) return;
    event->role=executor->current && executor->current->task?2u:1u;
    if (!call->top) return;
    uintptr_t first=(uintptr_t)call->config.entries,at=(uintptr_t)call->top->entry;
    uint64_t bytes=(uint64_t)call->config.entry_count*sizeof(*call->config.entries);
    if (at>=first && (uint64_t)(at-first)<bytes && (at-first)%sizeof(*call->config.entries)==0)
        event->entry=(unsigned)((at-first)/sizeof(*call->config.entries));
}
static void access_failure(AccessRuntimeRun *run,unsigned pass,XrXirCallStatus status) {
    CHECK(access_resource_status(status));
    run->failed[pass]=true;
    if (run->cost.failure==XR_XIR_CALL_READY) run->cost.failure=status;
    XrXirInstance *instance=run->instances[pass];
    if (!instance) return;
    /* A failed rebind has not accepted a new epoch. Its previous driver
     * failure remains authoritative until a real successful attach. */
    XrXirCallStatus epoch_failure=instance->executor->first_failure;
    if (epoch_failure!=XR_XIR_CALL_READY) {
        CHECK(epoch_failure==status || epoch_failure==run->observed[pass]);run->observed[pass]=epoch_failure;
    } else if (instance->executor->budget->exhausted) run->observed[pass]=XR_XIR_CALL_LIMIT;
    unsigned phase=cell_runtime_phase;
    access_phase(ACCESS_STICKY,pass);
    XrXirValue empty={0};
    if (instance->state==XR_XIR_INSTANCE_FAILED) {
        XrXirCallResult first={0},second={0};
        CHECK(xr_xir_instance_copy_failure(instance,&first)==status);
        CHECK(xr_xir_instance_copy_failure(instance,&second)==status && !memcmp(&first,&second,sizeof(first)));
        CHECK(xr_xir_instance_start(instance,run->entries->run,NULL,0)==status);
        CHECK(xr_xir_instance_poll_bounded(instance,1).outcome.status==status);
        CHECK(xr_xir_instance_take_result(instance,&empty)==XR_XIR_CALL_BAD_STATE);
        xr_xir_call_result_drop(&first);xr_xir_call_result_drop(&second);
    } else if (instance->call && (phase==ACCESS_POLL || phase==ACCESS_CONTROL)) {
        CHECK(xr_xir_instance_poll_bounded(instance,1).outcome.status==status);
        XrXirCallStatus completed=xr_xir_task_executor_completion_status(instance->executor);
        CHECK(xr_xir_instance_take_result(instance,&empty)==
            (completed==XR_XIR_CALL_READY?XR_XIR_CALL_BAD_STATE:completed));
    }
    CHECK(access_value_empty(&empty));
}
static XrXirCallStatus access_start(AccessRuntimeRun *run,unsigned pass,uint32_t entry) {
    XrXirInstance *instance=run->instances[pass];
    XrXirDomain *domain=instance->domain;
    XrXirInstanceState state=xr_xir_instance_state(instance);
    XrXirInstanceConfig config=instance->config;
    uint64_t epoch=instance->epoch;
    size_t blocks=runtime_live,bytes=runtime_bytes;
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(domain);
    bool prepare=!pass && !run->cost.prepare_end;
    if (prepare) {run->cost.prepare_begin=runtime_attempts;run->cost.prepare_before=before;}
    access_phase(ACCESS_START,pass);
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,NULL,0);
    if (access_resource_status(status)) {
        CHECK(instance->domain==domain && instance->budget.work_domain==domain && instance->epoch==epoch);
        CHECK(xr_xir_instance_state(instance)==state && !memcmp(&config,&instance->config,sizeof(config)));
        CHECK(runtime_live==blocks && runtime_bytes==bytes);
        XrXirDomainBudgetStats failed=xr_xir_domain_budget_stats(domain);
        CHECK(failed.requested_call_bytes>=before.requested_call_bytes && failed.work>=before.work);
        CHECK(failed.call_live==before.call_live && failed.metadata_live==before.metadata_live);
        if (prepare) run->cost.prepare_failed=failed;
        if (run->retry && !run->cost.recovered && status==XR_XIR_CALL_OOM && cell_runtime_hits==1) {
            CHECK(state==XR_XIR_INSTANCE_NEW || state==XR_XIR_INSTANCE_READY);run->cost.recovered=true;
            access_phase(ACCESS_RETRY,pass);
            status=xr_xir_instance_start(instance,entry,NULL,0);
            CHECK(instance->domain==domain && instance->budget.work_domain==domain);
            CHECK(!memcmp(&config,&instance->config,sizeof(config)));
            if (access_resource_status(status)) {
                CHECK(xr_xir_instance_state(instance)==state && instance->epoch==epoch);
                CHECK(runtime_live==blocks && runtime_bytes==bytes);
            }
        }
    }
    if (prepare) {run->cost.prepare_end=runtime_attempts;run->cost.prepare_after=xr_xir_domain_budget_stats(domain);}
    if (status==XR_XIR_CALL_READY) {
        CHECK(instance->executor->first_failure==XR_XIR_CALL_READY);run->observed[pass]=XR_XIR_CALL_READY;
    }
    return status;
}
static XrXirCallStatus access_drive(AccessRuntimeRun *run,unsigned pass) {
    access_phase(ACCESS_POLL,pass);
    XrXirCallStatus status;
    do {++cell_runtime_poll;CHECK(cell_runtime_poll);status=xr_xir_instance_poll_bounded(run->instances[pass],1).outcome.status;}
    while (status==XR_XIR_CALL_READY);
    return status;
}
static bool access_entry(AccessRuntimeRun *run,unsigned pass,uint32_t entry,XrXirValue *output,
    XrXirCallStatus expected) {
    CHECK(access_value_empty(output));
    XrXirCallStatus status=access_start(run,pass,entry);
    if (status==XR_XIR_CALL_READY) status=access_drive(run,pass);
    if (access_resource_status(status)) {access_failure(run,pass,status);return false;}
    CHECK(status==expected);
    if (status==XR_XIR_CALL_RETURNED) {
        access_phase(ACCESS_TRANSFER,pass);
        status=xr_xir_instance_take_result(run->instances[pass],output);
        if (access_resource_status(status)) {access_failure(run,pass,status);return false;}
        CHECK(status==XR_XIR_CALL_RETURNED && xr_xir_value_valid(output));
    } else {
        XrXirCallStatus completed=xr_xir_task_executor_completion_status(run->instances[pass]->executor);
        CHECK(completed==XR_XIR_CALL_READY || completed==expected);
        run->observed[pass]=expected;
        CHECK(xr_xir_instance_take_result(run->instances[pass],output)==
            (completed==XR_XIR_CALL_READY?XR_XIR_CALL_BAD_STATE:completed));
        CHECK(access_value_empty(output));
    }
    return true;
}
static bool access_normal_pass(AccessRuntimeRun *run,unsigned pass) {
    for (unsigned repeat=0;repeat<2;++repeat) {
        XrXirCallStatus expected=run->fixture->alias?XR_XIR_CALL_BAD_ARGUMENT:XR_XIR_CALL_RETURNED;
        if (!access_entry(run,pass,run->entries->run,&run->results[pass][repeat],expected)) return false;
        ++run->cost.results[pass];
        if (!run->fixture->alias) access_number(&run->results[pass][repeat],run->fixture->result);
        else {
            CHECK(run->entries->counter!=UINT32_MAX);
            if (!access_entry(run,pass,run->entries->counter,&run->counters[pass][repeat],XR_XIR_CALL_RETURNED)) return false;
            access_number(&run->counters[pass][repeat],0);++run->cost.getters[pass];
            run->observed[pass]=XR_XIR_CALL_READY;
        }
    }
    return true;
}
#endif // XIR_CLASS_ACCESS_RUNTIME_TRANSACTIONS_H
