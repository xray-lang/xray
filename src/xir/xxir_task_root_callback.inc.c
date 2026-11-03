/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_root_callback.inc.c - Owned sibling activation and retained host waits
 */
static bool task_core_runnable(const XrXirTaskExecutor *executor) {
    for (const XirTaskActivation *at=executor->ready_head;at;at=at->ready_next)
        if (!executor->callback.call || executor->stopping || at!=&executor->root) return true;
    return false;
}
XR_FUNC bool xir_task_callback_active(const XrXirTaskExecutor *executor) {
    return executor && task_executor_owner_current(executor) && executor->callback.call &&
        xir_instance_callback_owns(executor->config.instance,executor,executor->callback.call);
}
XR_FUNC XrXirCallStatus xir_task_callback_attach(XrXirTaskExecutor *executor,XrXirCall *call) {
    if (!executor || !call || !task_executor_owner_current(executor) ||
        !xir_instance_callback_owns(executor->config.instance,executor,call)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (executor->driving || task_executor_observing(executor)) return XR_XIR_CALL_BUSY;
    if (executor->stopping || executor->callback.call || !executor->root.call ||
        executor->first_failure!=XR_XIR_CALL_READY || executor->budget->exhausted ||
        executor->next_identity==UINT64_MAX) return XR_XIR_CALL_BAD_STATE;
    uint64_t ticket=executor->next_identity+1;
    XrXirExecutorBinding binding={executor,&executor->callback,executor->generation,ticket};
    XrXirCallStatus status=xr_xir_call_bind_executor(call,&binding);
    if (status!=XR_XIR_CALL_READY) return status;
    executor->callback=(XirTaskActivation){.call=call,.ticket=ticket,.entry=UINT32_MAX};
    executor->next_identity=ticket;
    /* The old external token and its deadline stay in their actual activation.
     * Only this selection changes; Parent's queued position is not consumed. */
    executor->external=NULL;executor->external_call_wake=0;
    task_core_enqueue(executor,&executor->callback);
    return XR_XIR_CALL_READY;
}
static void task_core_cancel_callback(XrXirTaskExecutor *executor) {
    XirTaskActivation *activation=&executor->callback;
    if (!activation->call) return;
    XrXirCallStatus status=xr_xir_call_request_cancel(activation->call);
    XR_CHECK(status==XR_XIR_CALL_CANCEL_REQUESTED || status==XR_XIR_CALL_BAD_STATE,
        "only the stopped owner's actual idle sibling can be cancelled");
    if (status==XR_XIR_CALL_CANCEL_REQUESTED && !activation->queued &&
        xr_xir_call_state(activation->call)!=XR_XIR_CALL_SUSPENDED)
        task_core_enqueue(executor,activation);
}
static void task_core_finish_callback(XrXirTaskExecutor *executor) {
    XirTaskActivation *activation=&executor->callback;
    XrXirCall *call=activation->call;
    XR_CHECK(call && xir_instance_callback_owns(executor->config.instance,executor,call),
        "a terminal callback retains its actual Instance sibling owner");
    XrXirCallResult outcome={0};
    XrXirCallStatus status=xr_xir_call_take_outcome(call,&outcome);
    XR_CHECK(status!=XR_XIR_CALL_READY && status!=XR_XIR_CALL_SUSPENDED &&
        xr_xir_call_result_valid(&outcome),"callback terminal moves before code or captures retire");
    bool failed=status!=XR_XIR_CALL_RETURNED;
    if (status==XR_XIR_CALL_CLEANUP_FAILED) {
        executor->callback_failed=true;
        task_core_capture_fatal(executor,&outcome);
    } else if (failed && executor->first_failure==XR_XIR_CALL_READY) {
        executor->callback_failed=true;
        executor->first_failure=status;executor->shutdown_status=status;
        xr_xir_call_result_drop(&executor->failure);
        xr_xir_call_result_move(&outcome,&executor->failure);
    }
    xr_xir_call_result_drop(&outcome);
    if (xr_xir_call_cleanup_incomplete(call)) executor->cleanup_incomplete=true;
    if (executor->external==activation) {executor->external=NULL;executor->external_call_wake=0;}
    *activation=(XirTaskActivation){0};
    XrXirCallStatus freed=xir_instance_callback_retire(executor->config.instance,call);
    if (freed!=XR_XIR_CALL_READY) {
        executor->cleanup_incomplete=true;task_core_record_status(executor,freed);
    }
    /* This is group shutdown, never a Task completion into Parent's inbox.
     * Parent and ordinary Tasks execute their real cancellation cleanup. */
    if (failed && !executor->stopping) (void)xr_xir_task_executor_stop(executor);
}
static bool task_core_host_token_matches(const XrXirTaskExecutor *executor,
    XirTaskActivation *activation,uint64_t wake) {
    if (!activation->call || activation->host_token!=wake || !activation->host_call_wake ||
        xr_xir_call_state(activation->call)!=XR_XIR_CALL_SUSPENDED) return false;
    XrXirExecutorBinding binding={(void *)executor,activation,executor->generation,activation->ticket};
    XrXirWaitRequest request={0};
    return xr_xir_call_executor_member(activation->call,&binding) &&
        xr_xir_call_wait_request(activation->call,activation->host_call_wake,&request)==XR_XIR_CALL_READY &&
        (request.kind==XR_XIR_WAIT_YIELD || request.kind==XR_XIR_WAIT_TIMER_MS ||
         request.kind==XR_XIR_WAIT_CHANNEL) && !request.subject && !request.generation && !request.ticket;
}
static XirTaskActivation *task_core_host_token(const XrXirTaskExecutor *executor,uint64_t wake) {
    if (!wake || wake>executor->host_wake) return NULL;
    if (task_core_host_token_matches(executor,(XirTaskActivation *)&executor->root,wake))
        return (XirTaskActivation *)&executor->root;
    if (task_core_host_token_matches(executor,(XirTaskActivation *)&executor->callback,wake))
        return (XirTaskActivation *)&executor->callback;
    for (XirTask *task=executor->active;task;task=task->active_next)
        if (task_core_host_token_matches(executor,&task->activation,wake)) return &task->activation;
    for (XirGenerator *generator=executor->generators;generator;generator=generator->active_next)
        if (task_core_host_token_matches(executor,&generator->activation,wake)) return &generator->activation;
    return NULL;
}
XR_FUNC XrXirCallStatus xir_task_host_wait_deadline(XrXirTaskExecutor *executor,
    uint64_t epoch,uint64_t wake,uint64_t now_ns,XirHostWaitDeadline *output) {
    if (!executor || !output || !task_executor_owner_current(executor)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (executor->driving || task_executor_observing(executor)) return XR_XIR_CALL_BUSY;
    if (!epoch || epoch!=executor->root_epoch || executor->stopping) return XR_XIR_CALL_BAD_STATE;
    XirTaskActivation *activation=task_core_host_token(executor,wake);
    XrXirWaitRequest request={0};
    if (!activation || xr_xir_call_wait_request(activation->call,activation->host_call_wake,&request)!=XR_XIR_CALL_READY ||
        (request.kind!=XR_XIR_WAIT_TIMER_MS && request.kind!=XR_XIR_WAIT_YIELD)) return XR_XIR_CALL_BAD_STATE;
    if (request.kind==XR_XIR_WAIT_YIELD) {
        *output=(XirHostWaitDeadline){0,0,true};return XR_XIR_CALL_READY;
    }
    if (!request.after_ms || request.after_ms>XR_XIR_TIMER_MAX_MS) return XR_XIR_CALL_BAD_STATE;
    if (!now_ns) return XR_XIR_CALL_HOST_ERROR;
    uint64_t duration=request.after_ms*UINT64_C(1000000);
    if (!activation->host_start_ns) {
        activation->host_start_ns=now_ns;activation->host_duration_ns=duration;
    } else if (now_ns<activation->host_start_ns || now_ns<activation->host_last_ns ||
        duration!=activation->host_duration_ns) return XR_XIR_CALL_HOST_ERROR;
    activation->host_last_ns=now_ns;
    *output=(XirHostWaitDeadline){activation->host_start_ns,duration,true};
    return XR_XIR_CALL_READY;
}
