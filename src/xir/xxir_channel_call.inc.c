/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_call.inc.c - Exact owned waits shared by VM and native execution
 *
 * KEY CONCEPT:
 *   Zero-capacity waits own a real peer intent. Buffered waits retain their
 *   evaluated operands in the caller and observe the frontier before trying.
 */
enum { CHANNEL_CALL_IDLE,CHANNEL_CALL_WATCH,CHANNEL_CALL_SEND,CHANNEL_CALL_RECV };
static bool channel_call_sending(XrXirChannelCallOperation operation) {
    return operation==XR_XIR_CHANNEL_CALL_SEND || operation==XR_XIR_CHANNEL_CALL_SEND_TIMEOUT;
}
static bool channel_call_timed(XrXirChannelCallOperation operation) {
    return operation==XR_XIR_CHANNEL_CALL_SEND_TIMEOUT || operation==XR_XIR_CHANNEL_CALL_RECV_TIMEOUT;
}
static XrXirCallStatus channel_call_value_status(XrXirValueStatus status) {
    return status==XR_XIR_VALUE_OK ? XR_XIR_CALL_READY : status==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM :
        status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_CALL_LIMIT :
        status==XR_XIR_VALUE_UNSUPPORTED ? XR_XIR_CALL_UNSUPPORTED : XR_XIR_CALL_BAD_ARGUMENT;
}
static bool channel_call_work(XrXirValueAdmission *admission) {
    if (!admission->work || !xr_xir_domain_work(admission->domain,1)) return false;
    --admission->work;return true;
}
#include "xxir_channel_call_timing.inc.c"
static XrXirCallStatus channel_call_default(const XrXirValue *value,XrXirType type,
    XrXirValueAdmission *admission) {
    if (!value) return XR_XIR_CALL_BAD_ARGUMENT;
    if (type==XR_XIR_UNIT) return boundary_value(*value,XR_XIR_UNIT) ?
        channel_call_work(admission) ? XR_XIR_CALL_READY : XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_ARGUMENT;
    return channel_call_value_status(xr_xir_value_admit(value,type,admission));
}
static XrXirCallStatus channel_call_receive_done(XrXirCallView *view,XrXirChannelCallState *state,
    XrXirChannelCallOperation operation,const XrXirValue *argument,XrXirType type,XrXirValue *output) {
    XrXirType expected=operation==XR_XIR_CHANNEL_CALL_RECV_OR && view->inbox.value.type==XR_XIR_UNIT ? XR_XIR_UNIT : type;
    XrXirCallStatus status=xr_xir_call_take_inbox(view,expected,output);
    if (status!=XR_XIR_CALL_READY) return status;
    *state=(XrXirChannelCallState){0};
    if (operation==XR_XIR_CHANNEL_CALL_RECV_OR && output->type==XR_XIR_UNIT && type!=XR_XIR_UNIT)
        return channel_call_value_status(xr_xir_value_copy(argument,output));
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus channel_call_resume(XrXirCallView *view,XrXirChannelCallState *state,
    XrXirChannelCallOperation operation,const XrXirValue *argument,XrXirType type,XrXirValue *output) {
    if (state->operation!=(uint32_t)operation || state->result_type!=type || state->waiting>CHANNEL_CALL_RECV ||
        (state->waiting==CHANNEL_CALL_SEND && !channel_call_sending(operation)) ||
        (state->waiting==CHANNEL_CALL_RECV && channel_call_sending(operation))) return XR_XIR_CALL_BAD_STATE;
    if (channel_call_timed(operation) && view->inbox.value.type==(uint32_t)type) {
        XrXirCallStatus status=xr_xir_call_take_inbox(view,type,output);
        if (status==XR_XIR_CALL_READY) { *state=(XrXirChannelCallState){0};channel_timing_clear(view->activation); }
        return status;
    }
    if (state->waiting==CHANNEL_CALL_RECV) return channel_call_receive_done(view,state,operation,argument,type,output);
    XrXirValue result={0};bool sending=state->waiting==CHANNEL_CALL_SEND;
    XrXirCallStatus status=xr_xir_call_take_inbox(view,sending ? XR_XIR_I64 : XR_XIR_UNIT,&result);
    if (status!=XR_XIR_CALL_READY) return status;
    if (channel_call_timed(operation) && !sending) state->waiting=CHANNEL_CALL_IDLE;
    else *state=(XrXirChannelCallState){0};
    if (!sending) return XR_XIR_CALL_BUSY;
    if (result.payload!=XR_XIR_CHANNEL_SEND_WAIT_SENT && result.payload!=XR_XIR_CHANNEL_SEND_WAIT_CLOSED) return XR_XIR_CALL_BAD_STATE;
    *output=result;return XR_XIR_CALL_READY;
}
static XrXirCallStatus channel_call_park_send(XrXirCallView *view,const XrXirValue *channel,
    const XrXirValue *argument,XrXirValue *output) {
    XrXirChannelSendWait *send=NULL;XrXirChannelSendWaitState state=XR_XIR_CHANNEL_SEND_WAIT_RETRY;
    XrXirValueStatus value=xr_xir_channel_rendezvous_send_begin(channel,argument,xr_xir_call_admission(view),&send,&state);
    if (value!=XR_XIR_VALUE_OK) return channel_call_value_status(value);
    if (state!=XR_XIR_CHANNEL_SEND_WAIT_PENDING) return XR_XIR_CALL_BUSY;
    XrXirCallStatus status=xr_xir_call_wait_channel_send(view,&send);
    if (status==XR_XIR_CALL_READY) {
        state=xr_xir_channel_send_wait_state(send);
        XR_CHECK(state==XR_XIR_CHANNEL_SEND_WAIT_SENT || state==XR_XIR_CHANNEL_SEND_WAIT_CLOSED,"ready send owns an exact terminal transport state");
        *output=(XrXirValue){XR_XIR_I64,0,state};
    }
    xr_xir_channel_send_wait_drop(send);return status;
}
static XrXirCallStatus channel_call_park_recv(XrXirCallView *view,const XrXirValue *channel,
    bool raw,XrXirType type,const XrXirValue *argument,XrXirValue *output) {
    XrXirChannelRecvWait *recv=NULL;XrXirChannelRecvWaitState state=XR_XIR_CHANNEL_RECV_WAIT_RETRY;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    XrXirValueStatus value=raw ? xr_xir_channel_rendezvous_recv_raw_begin(channel,admission,&recv,&state) :
        xr_xir_channel_rendezvous_recv_begin(channel,type,admission,&recv,&state);
    if (value!=XR_XIR_VALUE_OK) return channel_call_value_status(value);
    if (state!=XR_XIR_CHANNEL_RECV_WAIT_PENDING) return XR_XIR_CALL_BUSY;
    XrXirCallStatus status=xr_xir_call_wait_channel_recv(view,&recv);
    if (status==XR_XIR_CALL_READY) {
        state=xr_xir_channel_recv_wait_state(recv);
        XR_CHECK(xr_xir_channel_recv_wait_take(recv,output),"ready receive transfers one complete owned result");
        if (raw && state==XR_XIR_CHANNEL_RECV_WAIT_CLOSED && type!=XR_XIR_UNIT)
            status=channel_call_value_status(xr_xir_value_copy(argument,output));
    }
    xr_xir_channel_recv_wait_drop(recv);return status;
}
static XrXirCallStatus channel_call_try_send(XrXirValueAdmission *admission,const XrXirValue *channel,
    const XrXirValue *argument,XrXirValue *output,XrXirChannelState *outcome) {
    if (!channel_call_work(admission)) return XR_XIR_CALL_LIMIT;
    XrXirValue owned={0};XrXirValueStatus value=xr_xir_value_copy(argument,&owned);
    if (value==XR_XIR_VALUE_OK) value=xr_xir_channel_send_owned(channel,&owned,admission,outcome);
    xr_xir_value_drop(&owned);
    if (value!=XR_XIR_VALUE_OK) return channel_call_value_status(value);
    if (*outcome==XR_XIR_CHANNEL_SENT || *outcome==XR_XIR_CHANNEL_CLOSED)
        *output=(XrXirValue){XR_XIR_I64,0,*outcome==XR_XIR_CHANNEL_SENT ? XR_XIR_CHANNEL_SEND_WAIT_SENT : XR_XIR_CHANNEL_SEND_WAIT_CLOSED};
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_call_channel_step(XrXirCallView *view,XrXirChannelCallState *state,
    const XrXirChannelCallRequest *request,XrXirValue *output) {
    if (!request) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirChannelCallOperation operation=request->operation;
    const XrXirValue *channel=request->channel,*argument=request->argument;XrXirType type=request->result_type;
    bool sending=channel_call_sending(operation),timed=channel_call_timed(operation);
    bool receive=operation==XR_XIR_CHANNEL_CALL_RECV || operation==XR_XIR_CHANNEL_CALL_RECV_TIMEOUT;
    if (!state || !channel || !output || !boundary_value(*output,XR_XIR_UNIT) || output==channel || output==argument ||
        operation<XR_XIR_CHANNEL_CALL_SEND || operation>XR_XIR_CHANNEL_CALL_RECV_TIMEOUT ||
        (!receive && !argument) || (!timed && request->timeout_ms) ||
        (operation==XR_XIR_CHANNEL_CALL_SEND && type!=XR_XIR_I64)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus permission=xr_xir_call_execution_status(view);
    if (permission!=XR_XIR_CALL_READY) return permission;
    XrXirCall *call=view->activation;XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!timed && call->channel_timing.active) return XR_XIR_CALL_BAD_STATE;
    bool watch_resumed=state->waiting==CHANNEL_CALL_WATCH;
    if (timed && state->waiting) {
        if (!call->channel_timing.active) return XR_XIR_CALL_BAD_STATE;
        permission=channel_timing_begin(call,request,admission);if (permission!=XR_XIR_CALL_READY) return permission;
    }
    if (state->waiting) {
        XrXirCallStatus resumed=channel_call_resume(view,state,operation,argument,type,output);
        if (resumed!=XR_XIR_CALL_BUSY) return resumed;
    }
    if (timed) {
        bool fresh=!call->channel_timing.active;
        permission=channel_timing_begin(call,request,admission);if (permission!=XR_XIR_CALL_READY) return permission;
        if (!fresh && !watch_resumed) {
            bool due=false;permission=channel_timing_due(call,&due);if (permission!=XR_XIR_CALL_READY) return permission;
            if (due) { channel_timing_result(call,output,true);*state=(XrXirChannelCallState){0};return XR_XIR_CALL_READY; }
        }
    }
    XrXirChannelObservation observation={0};XrXirChannelState observed=XR_XIR_CHANNEL_RETRY;
    XrXirValueStatus value=xr_xir_channel_observe(channel,admission,&observation,&observed);
    if (value!=XR_XIR_VALUE_OK) return channel_call_value_status(value);
    if (observed==XR_XIR_CHANNEL_RETRY) {
        if (timed && !request->timeout_ms) { channel_timing_result(call,output,true);*state=(XrXirChannelCallState){0};return XR_XIR_CALL_READY; }
        return XR_XIR_CALL_BUSY;
    }
    if (operation==XR_XIR_CHANNEL_CALL_RECV_OR) {
        if (type!=xr_xir_channel_element(xr_xir_compile_type_arena_types(admission->arena),(XrXirType)channel->type)) return XR_XIR_CALL_BAD_ARGUMENT;
        permission=channel_call_default(argument,type,admission);if (permission!=XR_XIR_CALL_READY) return permission;
    }
    XrXirChannelState outcome=XR_XIR_CHANNEL_RETRY;
    if (sending) permission=channel_call_try_send(admission,channel,argument,output,&outcome);
    else {
        value=receive ? xr_xir_channel_receive_result(channel,type,admission,output,&outcome) :
            xr_xir_channel_receive(channel,admission,output,&outcome);
        permission=channel_call_value_status(value);
    }
    if (permission!=XR_XIR_CALL_READY) return permission;
    if (outcome==XR_XIR_CHANNEL_RETRY && !(timed && !request->timeout_ms)) return XR_XIR_CALL_BUSY;
    if (outcome==XR_XIR_CHANNEL_SENT || outcome==XR_XIR_CHANNEL_VALUE || outcome==XR_XIR_CHANNEL_CLOSED) {
        if (operation==XR_XIR_CHANNEL_CALL_RECV_OR && outcome==XR_XIR_CHANNEL_CLOSED && type!=XR_XIR_UNIT)
            return channel_call_value_status(xr_xir_value_copy(argument,output));
        if (timed) { channel_timing_result(call,output,false);*state=(XrXirChannelCallState){0}; }
        return XR_XIR_CALL_READY;
    }
    xr_xir_value_drop(output);
    if (timed) {
        bool due=false;permission=channel_timing_due(call,&due);if (permission!=XR_XIR_CALL_READY) return permission;
        if (due) { channel_timing_result(call,output,true);*state=(XrXirChannelCallState){0};return XR_XIR_CALL_READY; }
    }
    if (observation.capacity) permission=xr_xir_call_watch_channel(view,channel,observation.sequence);
    else permission=sending ? channel_call_park_send(view,channel,argument,output) :
        channel_call_park_recv(view,channel,operation==XR_XIR_CHANNEL_CALL_RECV_OR,type,argument,output);
    if (permission==XR_XIR_CALL_SUSPENDED) *state=(XrXirChannelCallState){observation.capacity ? CHANNEL_CALL_WATCH :
        sending ? CHANNEL_CALL_SEND : CHANNEL_CALL_RECV,(uint32_t)operation,type};
    else if (observation.capacity && permission==XR_XIR_CALL_READY) permission=XR_XIR_CALL_BUSY;
    else if (timed && permission==XR_XIR_CALL_READY) { channel_timing_result(call,output,false);*state=(XrXirChannelCallState){0}; }
    return permission;
}
