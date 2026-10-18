/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_iterator_wait.inc.c - FIFO producer demand and exact consumer wake
 */
/* Cached values and terminal outcomes need no producer execution authority.
 * Every ordinary advance, including an nth discard, uses the consumer role. */
static XrXirCallStatus generator_core_advance_permission(XrXirTaskExecutor *executor,
    XirGenerator *generator,const XrXirTaskWaitToken *token) {
    if(token->request.kind==XR_XIR_WAIT_ITERATOR_CLOSE) return XR_XIR_CALL_READY;
    XirTaskActivation *consumer=token->binding.activation;
    XirGenerator *parent=generator_core_activation(executor,consumer);
    bool child=consumer->task || (parent && parent->child);
    if(!child) {generator->child=false;return XR_XIR_CALL_READY;}
    XirEffectInvocationSelection selection={0};
    XrXirCallStatus selected=xr_xir_instance_iterator_select(consumer->call,token,&selection);
    if(selected!=XR_XIR_CALL_READY) return selected;
    if(selection.target!=generator->activation.entry || selection.producer!=generator->producer)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirProgramPermissions *permissions=executor->config.program->permissions;
    uint32_t entry=generator->activation.entry;
    if(!permissions || entry>=permissions->function_count || permissions->entries[entry].worker!=XR_XIR_OK)
        return XR_XIR_CALL_BAD_ARGUMENT;
    selected=xr_xir_call_iterator_prepare(consumer->call,token);
    if(selected!=XR_XIR_CALL_READY) return selected;
    generator->child=true;return XR_XIR_CALL_READY;
}
static void generator_core_complete_consumer(XrXirTaskExecutor *executor,XirGenerator *generator) {
    XrXirCall *call=generator->consumer;if(!call) return;
    XrXirTaskWaitToken token={0};
    XR_CHECK(xr_xir_call_iterator_wait_token(call,&token) && token.binding.owner==executor &&
        token.binding.generation==executor->generation,"a producer wakes only its exact live consumer");
    XirTaskActivation *activation=token.binding.activation;
    XR_CHECK(activation && activation->call==call && activation->ticket==token.binding.ticket && !activation->queued,
        "a parked consumer owns one activation and one pending wake");
    XrXirCallStatus status=xr_xir_call_complete_iterator_wait(call,&token);
    if(status==XR_XIR_CALL_SUSPENDED) {
        XR_CHECK(generator->consumer==call && generator->call &&
            xr_xir_call_state(generator->call)==XR_XIR_CALL_READY && !generator->activation.queued,
            "an intermediate nth discard retains its exact consumer and readies only its producer");
        XR_CHECK(xr_xir_call_iterator_wait_token(call,&token),
            "an intermediate nth demand refreshes its decremented exact token");
        status=generator_core_advance_permission(executor,generator,&token);
        if(status==XR_XIR_CALL_READY) {task_core_enqueue(executor,&generator->activation);return;}
    }
    if(status!=XR_XIR_CALL_READY) status=xr_xir_call_fail_iterator_wait(call,&token,status);
    XR_CHECK(status==XR_XIR_CALL_READY,"producer completion delivers or faults one consumer");
    task_core_record_failure(executor,activation);task_core_enqueue(executor,activation);
}
static void generator_core_register_consumer(XrXirTaskExecutor *executor,XrXirCall *call) {
    XrXirTaskWaitToken token={0};
    XR_CHECK(xr_xir_call_iterator_wait_token(call,&token),"an Iterator demand retains its exact request");
    XrXirCallStatus status=xr_xir_call_link_iterator_wait(call,&token);
    if(status==XR_XIR_CALL_RETURNED) {
        generator_core_complete_consumer(executor,(XirGenerator *)(uintptr_t)token.request.subject);return;
    }
    if(status==XR_XIR_CALL_SUSPENDED) {
        XirGenerator *generator=(XirGenerator *)(uintptr_t)token.request.subject;
        status=generator_core_advance_permission(executor,generator,&token);
        if(status==XR_XIR_CALL_READY) {
            if(xr_xir_call_state(generator->call)==XR_XIR_CALL_READY && !generator->activation.queued)
                task_core_enqueue(executor,&generator->activation);
            return;
        }
    }
    XR_CHECK(xr_xir_call_fail_iterator_wait(call,&token,status)==XR_XIR_CALL_READY,
        "rejected demand faults one consumer and releases only its exact producer lease");
    task_core_record_failure(executor,token.binding.activation);task_core_enqueue(executor,token.binding.activation);
}
static bool task_core_internal_wait(XrXirCall *call,XrXirTaskWaitToken *token) {
    return xr_xir_call_task_wait_token(call,token) || xr_xir_call_iterator_wait_token(call,token);
}
static void task_core_fail_internal_wait(XrXirTaskExecutor *executor,XirTaskActivation *activation,
    XrXirCallStatus reason) {
    XrXirTaskWaitToken token={0};
    XR_CHECK(task_core_internal_wait(activation->call,&token),"a closed dependency retains its exact waiter");
    XrXirCallStatus status=token.request.kind==XR_XIR_WAIT_TASK ?
        xr_xir_call_fail_task_wait(activation->call,&token,reason) :
        xr_xir_call_fail_iterator_wait(activation->call,&token,reason);
    XR_CHECK(status==XR_XIR_CALL_READY,"closed dependencies unlink before their owned handles are released");
    task_core_record_failure(executor,activation);task_core_enqueue(executor,activation);
}
