/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_iterator_wait.inc.c - Authenticated single-consumer owner transfer
 *
 * KEY CONCEPT:
 *   Only an exact owner, activation and wake can transfer one producer result.
 */
XR_FUNC bool xr_xir_call_iterator_wait_token(const XrXirCall *call,XrXirTaskWaitToken *output) {
    if(!call_owner_current(call) || !call || !output || call->driving || call_observing(call) ||
        call->result.status!=XR_XIR_CALL_SUSPENDED || !iterator_wait_kind(call->wait.kind) ||
        !call->executor_owner || !call->executor_activation) return false;
    *output=(XrXirTaskWaitToken){{call->executor_owner,call->executor_activation,
        call->executor_generation,call->executor_ticket},call->result.wake,call->wait};return true;
}
static bool iterator_wait_matches(const XrXirCall *call,const XrXirTaskWaitToken *token) {
    if(!call || !token) return false;
    const XrXirWaitRequest *wait=&token->request;
    return xr_xir_call_executor_member(call,&token->binding) && call->top &&
        (!call->cancel_requested || (wait->kind==XR_XIR_WAIT_ITERATOR_CLOSE && cleanup_active(call))) &&
        call->result.status==XR_XIR_CALL_SUSPENDED && token->wake && call->result.wake==token->wake &&
        iterator_wait_kind(wait->kind) && call->wait.kind==wait->kind && !wait->reserved && call->wait.after_ms==wait->after_ms &&
        (wait->kind==XR_XIR_WAIT_ITERATOR_NTH || !wait->after_ms) &&
        call->wait.subject==wait->subject && call->wait.generation==wait->generation &&
        call->wait.ticket==wait->ticket && call->waiting_task.type;
}
XR_FUNC XrXirCallStatus xr_xir_call_link_iterator_wait(XrXirCall *call,const XrXirTaskWaitToken *token) {
    if(!call_owner_current(call) || !call || !token) return XR_XIR_CALL_BAD_ARGUMENT;
    if(call->driving || call_observing(call)) return XR_XIR_CALL_BUSY;
    if(!iterator_wait_matches(call,token) || call->wait_linked) return XR_XIR_CALL_BAD_STATE;
    if(!xr_xir_domain_work(call->config.admission.domain,1)) return XR_XIR_CALL_LIMIT;
    XirGenerator *generator=(XirGenerator *)(uintptr_t)call->waiting_task.payload;
    if(generator->generation!=token->binding.generation || generator->identity!=token->request.ticket ||
        generator->identity==token->binding.ticket || generator->consumer) return XR_XIR_CALL_BAD_STATE;
    bool terminal=atomic_load_explicit(&generator->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL;
    if(!terminal && generator->executor!=token->binding.owner) return XR_XIR_CALL_BAD_STATE;
    generator->consumer=call;call->wait_linked=true;
    if(token->request.kind==XR_XIR_WAIT_ITERATOR_CLOSE && !terminal) {
        atomic_store_explicit(&generator->orphaned,true,memory_order_release);return XR_XIR_CALL_SUSPENDED;
    }
    return terminal || xr_xir_call_state(generator->call)==XR_XIR_CALL_YIELDED ?
        XR_XIR_CALL_RETURNED : XR_XIR_CALL_SUSPENDED;
}
XR_FUNC XrXirCallStatus xr_xir_call_complete_iterator_wait(XrXirCall *call,const XrXirTaskWaitToken *token) {
    if(!call_owner_current(call) || !call || !token) return XR_XIR_CALL_BAD_ARGUMENT;
    if(call->driving || call_observing(call)) return XR_XIR_CALL_BUSY;
    if(!iterator_wait_matches(call,token) || !call->wait_linked) return XR_XIR_CALL_BAD_STATE;
    XirGenerator *generator=(XirGenerator *)(uintptr_t)call->waiting_task.payload;
    if(generator->consumer!=call || generator->generation!=token->request.generation ||
        generator->identity!=token->request.ticket) return XR_XIR_CALL_BAD_STATE;
    XrXirCallResult outcome={0};
    bool terminal=atomic_load_explicit(&generator->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL;
    if(!terminal && token->request.kind==XR_XIR_WAIT_ITERATOR_CLOSE) return XR_XIR_CALL_BAD_STATE;
    if(terminal) {
        if(token->request.kind==XR_XIR_WAIT_ITERATOR_CLOSE && generator->outcome.status==XR_XIR_CALL_THROWN)
            return XR_XIR_CALL_BAD_STATE;
        if(generator->outcome.status!=XR_XIR_CALL_RETURNED && generator->outcome.status!=XR_XIR_CALL_CANCELLED) {
            XrXirValueStatus copied=xr_xir_call_result_copy(&generator->outcome,&outcome);
            if(copied!=XR_XIR_VALUE_OK) return copied==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_LIMIT;
        } else if(token->request.kind==XR_XIR_WAIT_ITERATOR_CLOSE) outcome.status=XR_XIR_CALL_RETURNED;
        else if(token->request.kind==XR_XIR_WAIT_ITERATOR_HAS_NEXT) {
            outcome.status=XR_XIR_CALL_RETURNED;outcome.value=(XrXirValue){XR_XIR_BOOL,0,0};
        }
        else {
            outcome.status=XR_XIR_CALL_RUNTIME_PANIC;outcome.panic.detail.code=XR_XIR_PANIC_ITERATOR_EXHAUSTED;
        }
    } else {
        if(xr_xir_call_state(generator->call)!=XR_XIR_CALL_YIELDED) return XR_XIR_CALL_BAD_STATE;
        outcome.status=XR_XIR_CALL_RETURNED;
        if(token->request.kind==XR_XIR_WAIT_ITERATOR_HAS_NEXT) outcome.value=(XrXirValue){XR_XIR_BOOL,0,1};
        else if(xr_xir_call_generator_take(generator->call,&outcome.value)!=XR_XIR_CALL_YIELDED)
            return XR_XIR_CALL_BAD_STATE;
    }
    if(!terminal && token->request.kind==XR_XIR_WAIT_ITERATOR_NTH && call->wait.after_ms) {
        xr_xir_call_result_drop(&outcome);--call->wait.after_ms;return XR_XIR_CALL_SUSPENDED;
    }
    unlink_task_wait(call,false);return complete_owned_wait(call,&outcome);
}
XR_FUNC XrXirCallStatus xr_xir_call_fail_iterator_wait(XrXirCall *call,const XrXirTaskWaitToken *token,
    XrXirCallStatus reason) {
    if(!call_owner_current(call) || !call || !token) return XR_XIR_CALL_BAD_ARGUMENT;
    if(call->driving || call_observing(call)) return XR_XIR_CALL_BUSY;
    if(!iterator_wait_matches(call,token) || (reason!=XR_XIR_CALL_OOM && reason!=XR_XIR_CALL_LIMIT &&
        reason!=XR_XIR_CALL_BAD_STATE && reason!=XR_XIR_CALL_BAD_ARGUMENT && reason!=XR_XIR_CALL_UNSUPPORTED))
        return XR_XIR_CALL_BAD_STATE;
    unlink_task_wait(call,true);xr_xir_value_drop(&call->waiting_task);
    call->wait=(XrXirWaitRequest){0};call->result=call_result(XR_XIR_CALL_READY);
    request_execution_failure(call,reason);return XR_XIR_CALL_READY;
}
