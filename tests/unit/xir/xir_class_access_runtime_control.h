/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_runtime_control.h - Cancellation of genuine projected Class loans
 */
#ifndef XIR_CLASS_ACCESS_RUNTIME_CONTROL_H
#define XIR_CLASS_ACCESS_RUNTIME_CONTROL_H
typedef struct AccessCloseObservation {
    XrXirInstance *instance;
    XrXirDomain *domain;
    XrXirCallBudget budget;
    XrXirCallStatus completed, first, shutdown;
    bool executor_released, instance_released;
} AccessCloseObservation;
static AccessCloseObservation access_close_observation;
/* The allocator owns the still-live implementation object. Copy its final
 * budget before physical release, without allocating or re-entering its Domain. */
static void access_runtime_before_free(const void *pointer) {
    AccessCloseObservation *observed=&access_close_observation;
    XrXirInstance *instance=observed->instance;
    if (!instance || !pointer) return;
    if (!observed->executor_released && pointer==instance->executor) {
        const XrXirTaskExecutor *executor=instance->executor;
        CHECK(executor->config.domain==observed->domain && executor->budget==&instance->budget);
        CHECK(!executor->driving && !executor->active && !executor->ready_head && !executor->current);
        CHECK(!executor->root.call || (executor->root.call==instance->call && !executor->root.call->top));
        observed->completed=task_core_completion_status(executor);
        observed->first=executor->first_failure;observed->shutdown=executor->shutdown_status;
        observed->executor_released=true;
    } else if (pointer==instance) {
        CHECK(observed->executor_released && !observed->instance_released);
        CHECK(!instance->executor && !instance->call && instance->domain==observed->domain);
        CHECK(instance->budget.work_domain==observed->domain && !instance->budget.live_bytes);
        CHECK(instance->budget.allocations==instance->budget.frees &&
            instance->budget.release_tickets==instance->budget.released_frames);
        observed->budget=instance->budget;observed->instance_released=true;
        observed->instance=NULL;
    }
}
static const XrXirValue *access_call_projection(const XrXirCall *call) {
    if (!call) return NULL;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(call->config.admission.arena);
    uint32_t depth=0;
    for (const CallFrame *frame=call->top;frame;frame=frame->parent) {
        CHECK(++depth<=call->config.depth_limit);
        for (uint32_t i=0;i<frame->loan_count;++i) {
            const CallCellLoan *record=&frame->loans[i];
            CHECK(record->parameter<frame->entry->parameter_count && frame->arguments);
            const XrXirValue *value=&frame->arguments[record->parameter];
            const XrXirValue *root=xr_xir_cell_canonical_root(value);
            if (!root || !xr_xir_type_is_class(types,(XrXirType)root->type)) continue;
            CHECK(xr_xir_cell_is_projection(value) && !xr_xir_cell_unborrowed(value));
            CHECK(record->loan.authority.activation==call && record->loan.authority.frame==frame &&
                record->loan.authority.epoch==frame->epoch && record->loan.cell && record->loan.scope);
            return value;
        }
    }
    return NULL;
}
static const XrXirValue *access_instance_projection(XrXirInstance *instance) {
    const XrXirValue *value=access_call_projection(instance->call);
    if (value) return value;
    CHECK(instance->executor && !instance->executor->driving && !instance->executor->current);
    uint32_t count=0;
    for (XirTask *task=instance->executor->active;task;task=task->active_next) {
        CHECK(++count);
        value=access_call_projection(task->call);
        if (value) return value;
    }
    return NULL;
}
/* Descriptor lifetime is separate from its live frame loan. The retained
 * descriptor pins this exact canonical identity but grants no public access. */
static void access_projection_released(const AccessRuntimeRun *run) {
    CHECK(run && run->domains[0] && run->projection_owner);
    XirCell *descriptor=cell_owner(&run->projection);
    CHECK(descriptor && descriptor->projection_count && descriptor->initialized);
    XirObject *owner=cell_root_object(descriptor);
    CHECK(owner && owner==run->projection_owner && owner->kind==XIR_OBJECT_CLASS);
    CHECK(descriptor->object.domain==run->domains[0] && owner->domain==run->domains[0] &&
        descriptor->object.arena==owner->arena && xr_xir_cell_in_domain(&run->projection,run->domains[0]));
    XrXirCellLoan **loans=cell_root_loans(owner);
    CHECK(loans && !*loans);
    uint32_t state=class_origin_state((const XirClassObject *)owner);
    CHECK(!(state&(XIR_CLASS_ORIGIN_BORROWED|XIR_CLASS_ORIGIN_PREPARED)));
    CHECK(xr_xir_cell_is_projection(&run->projection) && !xr_xir_cell_unborrowed(&run->projection));
    size_t attempts=runtime_attempts;
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(run->domains[0]);
    XrXirValue empty={0};XrXirCellLoan denied={0};
    CHECK(xr_xir_cell_read(&run->projection,&empty)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(access_value_empty(&empty));
    CHECK(xr_xir_cell_loan_prepare(&run->projection,NULL,&denied)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(cell_loan_empty(&denied) && runtime_attempts==attempts && !*loans);
    CHECK(class_origin_state((const XirClassObject *)owner)==state);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(run->domains[0]);
    CHECK(before.requested_bytes==after.requested_bytes &&
        before.requested_limit==after.requested_limit &&
        before.work==after.work &&
        before.work_limit==after.work_limit &&
        before.bound==after.bound &&
        before.requested_call_bytes==after.requested_call_bytes &&
        before.requested_call_limit==after.requested_call_limit &&
        before.metadata_live==after.metadata_live &&
        before.metadata_peak==after.metadata_peak &&
        before.metadata_limit==after.metadata_limit &&
        before.metadata_allocations==after.metadata_allocations &&
        before.metadata_frees==after.metadata_frees &&
        before.call_live==after.call_live &&
        before.call_peak==after.call_peak &&
        before.call_limit==after.call_limit &&
        before.call_allocations==after.call_allocations &&
        before.call_frees==after.call_frees);
}
static bool access_control(AccessRuntimeRun *run) {
    CHECK(!run->fixture->alias && run->mode>ACCESS_RUN);
    XrXirCallStatus status=access_start(run,0,run->entries->run);
    if (access_resource_status(status)) {access_failure(run,0,status);return false;}
    CHECK(status==XR_XIR_CALL_READY);
    const XrXirValue *projection=NULL;
    access_phase(ACCESS_POLL,0);
    while (!projection) {
        ++cell_runtime_poll;CHECK(cell_runtime_poll);
        status=xr_xir_instance_poll_bounded(run->instances[0],1).outcome.status;
        if (access_resource_status(status)) {access_failure(run,0,status);return false;}
        CHECK(status==XR_XIR_CALL_READY);
        projection=access_instance_projection(run->instances[0]);
    }
    CHECK(xr_xir_value_copy(projection,&run->projection)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_same_owner(projection,&run->projection));
    run->projection_owner=cell_root_object(cell_owner(projection));
    CHECK(run->projection_owner);
    ++run->cost.loans;run->cost.controlled=true;
    access_phase(ACCESS_CONTROL,0);
    if (run->mode==ACCESS_CLOSE_ACTIVE) return true;
    if (run->mode==ACCESS_CANCEL) {
        size_t attempts=runtime_attempts;
        CHECK(xr_xir_instance_cancel_current(run->instances[0])==XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(runtime_attempts==attempts);
        status=access_drive(run,0);
        if (access_resource_status(status)) {access_failure(run,0,status);return false;}
        CHECK(status==XR_XIR_CALL_CANCELLED);
        CHECK(xr_xir_task_executor_root_idle(run->instances[0]->executor) &&
            !run->instances[0]->executor->active && !run->instances[0]->executor->current &&
            !run->instances[0]->call->top);
        access_projection_released(run);
        CHECK(access_value_empty(&run->results[0][0]));
        return access_normal_pass(run,0);
    }
    CHECK(run->mode==ACCESS_STOP);
    status=xr_xir_instance_stop(run->instances[0]);
    if (access_resource_status(status)) {access_failure(run,0,status);return false;}
    CHECK(status==XR_XIR_CALL_READY);
    access_projection_released(run);
    size_t attempts=runtime_attempts;
    CHECK(xr_xir_instance_start(run->instances[0],run->entries->run,NULL,0)==XR_XIR_CALL_BAD_STATE);
    CHECK(runtime_attempts==attempts);
    return true;
}
static void access_close(AccessRuntimeRun *run,unsigned pass) {
    XrXirInstance *instance=run->instances[pass];
    if (!instance) return;
    bool root_idle=xr_xir_task_executor_root_idle(instance->executor);
    bool idle=root_idle && !instance->executor->active &&
        !instance->executor->ready_head && (!instance->call || !instance->call->top);
    XrXirCallStatus completed=root_idle?xr_xir_task_executor_completion_status(instance->executor):
        task_core_completion_status(instance->executor);
    if (completed!=XR_XIR_CALL_READY) {
        CHECK(completed==run->observed[pass]);
        if (instance->executor->first_failure!=XR_XIR_CALL_READY)
            CHECK(instance->executor->first_failure==completed);
        else if (instance->executor->shutdown_status!=XR_XIR_CALL_READY)
            CHECK(instance->executor->shutdown_status==completed);
        else CHECK(completed==XR_XIR_CALL_LIMIT && instance->budget.exhausted);
    }
    CHECK(!access_close_observation.instance);
    access_close_observation=(AccessCloseObservation){.instance=instance,.domain=run->domains[pass]};
    size_t hits=run->injected[pass];access_phase(ACCESS_CLOSE,pass);
    XrXirCallStatus status=xr_xir_instance_free(instance);run->instances[pass]=NULL;
    AccessCloseObservation closed=access_close_observation;
    CHECK(closed.executor_released && closed.instance_released && !closed.instance);
    CHECK(status==closed.completed);
    if (idle) CHECK(status==completed);
    else if (status!=XR_XIR_CALL_READY) {
        CHECK(access_resource_status(status));
        if (status==XR_XIR_CALL_OOM) CHECK(run->injected[pass]>hits || run->observed[pass]==status);
        else CHECK(closed.first==XR_XIR_CALL_LIMIT || closed.shutdown==XR_XIR_CALL_LIMIT ||
            closed.budget.exhausted);
    }
    if (status!=XR_XIR_CALL_READY)
        printf("CLASS_ACCESS_CLOSE owner=%u before=%u final=%u first=%u shutdown=%u exhausted=%u actual=%u physicalReleased=1\n",
            pass,(unsigned)completed,(unsigned)closed.completed,(unsigned)closed.first,
            (unsigned)closed.shutdown,(unsigned)closed.budget.exhausted,(unsigned)status);
    access_close_observation=(AccessCloseObservation){0};
    if (access_resource_status(status)) {
        if (run->cost.failure==XR_XIR_CALL_READY) run->cost.failure=status;
        run->observed[pass]=status;
    }
}
#endif // XIR_CLASS_ACCESS_RUNTIME_CONTROL_H
