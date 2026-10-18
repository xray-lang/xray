/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_channel.inc.c - Authenticated owned Channel suspension and wake
 *
 * KEY CONCEPT:
 *   A queue event owns no Call pointer and a host wake cannot authorize it.
 */
XR_FUNC XrXirCallStatus xr_xir_call_watch_channel(XrXirCallView *view,
    const XrXirValue *channel, uint64_t sequence) {
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    if (!admission) return XR_XIR_CALL_BAD_STATE;
    XrXirCall *call = view->activation;
    if (call->channel_watch || call->channel_send || call->channel_recv || call->wait.kind != XR_XIR_WAIT_NONE ||
        call->result.status != XR_XIR_CALL_READY) return XR_XIR_CALL_BAD_STATE;
    if (cleanup_active(call)) return XR_XIR_CALL_DEFER_ASYNC;
    if (call->cancel_requested) return XR_XIR_CALL_CANCELLED;
    XrXirChannelWatch *watch = NULL;
    XrXirChannelWatchState state = XR_XIR_CHANNEL_WATCH_RETRY;
    XrXirValueStatus status = xr_xir_channel_watch_begin(channel, sequence, admission, &watch, &state);
    if (status != XR_XIR_VALUE_OK) return status == XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM :
        status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_ARGUMENT;
    if (state == XR_XIR_CHANNEL_WATCH_READY) return XR_XIR_CALL_READY;
    if (state == XR_XIR_CHANNEL_WATCH_RETRY) return XR_XIR_CALL_BUSY;
    XR_CHECK(watch && !call->channel_watch, "one active frame publishes one owned Channel watch");
    call->channel_watch = watch; call->channel_epoch = call->top->epoch;
    return XR_XIR_CALL_SUSPENDED;
}
XR_FUNC XrXirCallStatus xr_xir_call_wait_channel_send(XrXirCallView *view,XrXirChannelSendWait **owner) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission) return XR_XIR_CALL_BAD_STATE;
    if (!owner || !*owner || !xr_xir_channel_send_wait_owner(*owner,admission)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCall *call=view->activation;
    if (call->channel_watch || call->channel_send || call->channel_recv || call->wait.kind!=XR_XIR_WAIT_NONE ||
        call->result.status!=XR_XIR_CALL_READY) return XR_XIR_CALL_BAD_STATE;
    if (cleanup_active(call)) return XR_XIR_CALL_DEFER_ASYNC;
    if (call->cancel_requested) return XR_XIR_CALL_CANCELLED;
    XrXirChannelSendWaitState state=xr_xir_channel_send_wait_state(*owner);
    if (state==XR_XIR_CHANNEL_SEND_WAIT_SENT || state==XR_XIR_CHANNEL_SEND_WAIT_CLOSED) return XR_XIR_CALL_READY;
    if (state!=XR_XIR_CHANNEL_SEND_WAIT_PENDING) return XR_XIR_CALL_BAD_STATE;
    call->channel_send=*owner;*owner=NULL;call->channel_epoch=call->top->epoch;
    return XR_XIR_CALL_SUSPENDED;
}
XR_FUNC XrXirCallStatus xr_xir_call_wait_channel_recv(XrXirCallView *view,XrXirChannelRecvWait **owner) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission) return XR_XIR_CALL_BAD_STATE;
    if (!owner || !*owner || !xr_xir_channel_recv_wait_owner(*owner,admission)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCall *call=view->activation;
    if (call->channel_watch || call->channel_send || call->channel_recv || call->wait.kind!=XR_XIR_WAIT_NONE ||
        call->result.status!=XR_XIR_CALL_READY) return XR_XIR_CALL_BAD_STATE;
    if (cleanup_active(call)) return XR_XIR_CALL_DEFER_ASYNC;
    if (call->cancel_requested) return XR_XIR_CALL_CANCELLED;
    XrXirChannelRecvWaitState state=xr_xir_channel_recv_wait_state(*owner);
    if (state==XR_XIR_CHANNEL_RECV_WAIT_VALUE || state==XR_XIR_CHANNEL_RECV_WAIT_CLOSED) return XR_XIR_CALL_READY;
    if (state!=XR_XIR_CHANNEL_RECV_WAIT_PENDING) return XR_XIR_CALL_BAD_STATE;
    call->channel_recv=*owner;*owner=NULL;call->channel_epoch=call->top->epoch;
    return XR_XIR_CALL_SUSPENDED;
}
static unsigned channel_wait_owner_count(const XrXirCall *call) {
    return (call->channel_watch ? 1u : 0u)+(call->channel_send ? 1u : 0u)+(call->channel_recv ? 1u : 0u);
}
XR_FUNC bool xr_xir_call_channel_waiting(const XrXirCall *call) {
    return call && call_owner_current(call) && call->result.status == XR_XIR_CALL_SUSPENDED &&
        call->wait.kind == XR_XIR_WAIT_CHANNEL && channel_wait_owner_count(call)==1 && call->top &&
        call->channel_epoch == call->top->epoch;
}
XR_FUNC bool xr_xir_call_channel_ready(const XrXirCall *call) {
    if (!xr_xir_call_channel_waiting(call)) return false;
    if (call->channel_timing.active && call->channel_timing.expired) return true;
    if (call->channel_watch) return xr_xir_channel_watch_ready(call->channel_watch);
    if (call->channel_recv) {
        XrXirChannelRecvWaitState state=xr_xir_channel_recv_wait_state(call->channel_recv);
        return state==XR_XIR_CHANNEL_RECV_WAIT_VALUE || state==XR_XIR_CHANNEL_RECV_WAIT_CLOSED;
    }
    XrXirChannelSendWaitState state=xr_xir_channel_send_wait_state(call->channel_send);
    return state==XR_XIR_CHANNEL_SEND_WAIT_SENT || state==XR_XIR_CHANNEL_SEND_WAIT_CLOSED;
}
static void accept_channel_wait(XrXirCall *call, const XrXirAction *action) {
    if (!boundary_value(action->value, XR_XIR_UNIT) || action->flags ||
        action->callee || action->arguments || action->argument_count) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
    }
    if (cleanup_active(call)) {
        clear_channel_wait(call);
        XrXirCallResult result = call_result(XR_XIR_CALL_DEFER_ASYNC);
        result.panic.detail.code = XR_XIR_PANIC_DEFER_ASYNC;
        request_exit(call, &result, false); return;
    }
    if (channel_wait_owner_count(call)!=1 || call->channel_epoch != call->top->epoch || call->next_wake == UINT64_MAX) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
    }
    call->result = call_result(XR_XIR_CALL_SUSPENDED); call->result.wake = ++call->next_wake;
    call->wait = (XrXirWaitRequest){.kind = XR_XIR_WAIT_CHANNEL};
}
static void complete_channel_wait(XrXirCall *call) {
    XR_CHECK(xr_xir_call_channel_ready(call), "only an observed queue event completes the current Channel wait");
    bool permitted = !call->budget || (!call->budget->exhausted && xr_xir_domain_work(call->budget->work_domain, 1));
    bool sending=call->channel_send!=NULL;
    bool receiving=call->channel_recv!=NULL,expired=call->channel_timing.active && call->channel_timing.expired;
    bool timed=call->channel_timing.active;XrXirValue received={0};
    XrXirChannelSendWaitState state=sending ? xr_xir_channel_send_wait_state(call->channel_send) : XR_XIR_CHANNEL_SEND_WAIT_RETRY;
    if (permitted && !expired && receiving && !xr_xir_channel_recv_wait_take(call->channel_recv,&received)) {
        abort_frames(call,XR_XIR_CALL_BAD_STATE);return;
    }
    if (permitted && expired) { received=call->channel_timing.timeout;call->channel_timing.timeout=(XrXirValue){0}; }
    else if (permitted && timed && sending) {
        XrXirValue *prepared=state==XR_XIR_CHANNEL_SEND_WAIT_SENT ? &call->channel_timing.sent : &call->channel_timing.closed;
        received=*prepared;*prepared=(XrXirValue){0};
    }
    release_channel_wait(call); call->result = call_result(XR_XIR_CALL_READY);
    if (!permitted) channel_timing_clear(call);
    if (!permitted) {
        call->budget->exhausted = true; request_execution_failure(call, XR_XIR_CALL_LIMIT); return;
    }
    if (call->budget) ++call->budget->transitions;
    xr_xir_call_result_drop(&call->top->inbox);
    call->top->inbox = call_result(XR_XIR_CALL_RETURNED);
    if (expired || (timed && sending) || receiving) call->top->inbox.value=received;
    else if (sending) call->top->inbox.value=(XrXirValue){XR_XIR_I64,0,state};
}
XR_FUNC XrXirCallStatus xr_xir_call_fail_channel_wait(XrXirCall *call,
    const XrXirExecutorBinding *binding, XrXirCallStatus reason) {
    if (!call_owner_current(call) || !call || !binding) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call)) return XR_XIR_CALL_BUSY;
    bool execution_failure=reason==XR_XIR_CALL_LIMIT || reason==XR_XIR_CALL_OOM || reason==XR_XIR_CALL_HOST_ERROR ||
        reason==XR_XIR_CALL_OUTPUT_ERROR || reason==XR_XIR_CALL_BAD_ARGUMENT || reason==XR_XIR_CALL_BAD_ABI ||
        reason==XR_XIR_CALL_BAD_STATE || reason==XR_XIR_CALL_UNSUPPORTED;
    if (!xr_xir_call_executor_member(call, binding) || !xr_xir_call_channel_waiting(call) || !execution_failure)
        return XR_XIR_CALL_BAD_STATE;
    clear_channel_wait(call); call->result = call_result(XR_XIR_CALL_READY);
    request_execution_failure(call, reason); return XR_XIR_CALL_READY;
}
