/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_call_timing.inc.c - Prepared native results and fixed host deadlines
 *
 * KEY CONCEPT:
 *   A clock read grants only a pending-intent CAS attempt. A peer that already
 *   claimed COMMITTING owns its result and cannot be undone by a late timer.
 */
static XrXirCallStatus channel_timing_clock(XrXirCall *call,uint64_t *nanoseconds) {
    const XrXirTimeProvider *provider=&call->config.time;
    if (!provider->abi_version) return XR_XIR_CALL_HOST_ERROR;
    if (call_observing(call)) return XR_XIR_CALL_BUSY;
    if (!channel_call_work(&call->config.admission)) return XR_XIR_CALL_LIMIT;
    call->time_reading=true;
    if (call->budget) call->budget->notifying=true;
    int64_t reading=0;XrXirTimeStatus status=provider->clock(provider->context,XR_XIR_CLOCK_MONOTONIC,&reading);
    if (call->budget) call->budget->notifying=false;
    call->time_reading=false;
    if (status!=XR_XIR_TIME_OK || reading<=0) return XR_XIR_CALL_HOST_ERROR;
    *nanoseconds=(uint64_t)reading;return XR_XIR_CALL_READY;
}
static XrXirCallStatus channel_timing_due(XrXirCall *call,bool *due) {
    CallChannelTiming *timing=&call->channel_timing;
    if (!timing->active || !call->top || timing->epoch!=call->top->epoch) return XR_XIR_CALL_BAD_STATE;
    if (!timing->duration_ns) { *due=true;return XR_XIR_CALL_READY; }
    uint64_t now=0;XrXirCallStatus status=channel_timing_clock(call,&now);
    if (status!=XR_XIR_CALL_READY) return status;
    if (now<timing->last_ns || now<timing->start_ns) return XR_XIR_CALL_HOST_ERROR;
    timing->last_ns=now;*due=now-timing->start_ns>=timing->duration_ns;return XR_XIR_CALL_READY;
}
static XrXirCallStatus channel_timing_begin(XrXirCall *call,const XrXirChannelCallRequest *request,
    XrXirValueAdmission *admission) {
    CallChannelTiming *timing=&call->channel_timing;
    if (timing->active) return call->top && timing->epoch==call->top->epoch &&
        timing->operation==(uint32_t)request->operation && timing->type==request->result_type &&
        timing->milliseconds==request->timeout_ms ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_STATE;
    if (request->timeout_ms<0 || (uint64_t)request->timeout_ms>XR_XIR_TIMER_MAX_MS) return XR_XIR_CALL_NUMERIC_RANGE;
    if (cleanup_active(call)) return XR_XIR_CALL_DEFER_ASYNC;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(admission->arena);
    XrXirType channel=(XrXirType)request->channel->type,type=request->result_type;
    if (!xr_xir_type_is_channel(types,channel) ||
        (channel_call_sending(request->operation) ? !xr_xir_nominal_native_send_result(types,type) :
            !xr_xir_nominal_native_recv(types,type,xr_xir_channel_element(types,channel)))) return XR_XIR_CALL_BAD_ARGUMENT;
    *timing=(CallChannelTiming){.epoch=call->top->epoch,.milliseconds=request->timeout_ms,
        .duration_ns=(uint64_t)request->timeout_ms*UINT64_C(1000000),.type=type,
        .operation=(uint32_t)request->operation,.active=true};
    XrXirValueStatus value=XR_XIR_VALUE_OK;
    if (channel_call_sending(request->operation)) {
        value=xr_xir_enum_new(type,0,NULL,0,admission,&timing->sent);
        if (value==XR_XIR_VALUE_OK) value=xr_xir_enum_new(type,3,NULL,0,admission,&timing->closed);
    }
    if (value==XR_XIR_VALUE_OK) value=xr_xir_enum_new(type,2,NULL,0,admission,&timing->timeout);
    XrXirCallStatus status=channel_call_value_status(value);
    if (status==XR_XIR_CALL_READY && timing->duration_ns) {
        status=channel_timing_clock(call,&timing->start_ns);timing->last_ns=timing->start_ns;
    }
    if (status!=XR_XIR_CALL_READY) channel_timing_clear(call);
    return status;
}
static void channel_timing_result(XrXirCall *call,XrXirValue *output,bool timeout) {
    CallChannelTiming *timing=&call->channel_timing;
    XR_CHECK(timing->active,"a timed result consumes its prepared owner");
    if (timeout) {
        XR_CHECK(boundary_value(*output,XR_XIR_UNIT),"timeout cannot overwrite a committed receive");
        *output=timing->timeout;timing->timeout=(XrXirValue){0};
    } else if (timing->operation==XR_XIR_CHANNEL_CALL_SEND_TIMEOUT) {
        XR_CHECK(output->type==XR_XIR_I64 && (output->payload==XR_XIR_CHANNEL_SEND_WAIT_SENT ||
            output->payload==XR_XIR_CHANNEL_SEND_WAIT_CLOSED),"timed send consumes one committed transport state");
        XrXirValue *result=output->payload==XR_XIR_CHANNEL_SEND_WAIT_SENT ? &timing->sent : &timing->closed;
        *output=*result;*result=(XrXirValue){0};
    }
    channel_timing_clear(call);
}
static XrXirCallStatus channel_timing_poll(XrXirCall *call) {
    if (!call->channel_timing.active || call->channel_timing.expired || xr_xir_call_channel_ready(call))
        return XR_XIR_CALL_READY;
    bool due=false;XrXirCallStatus status=channel_timing_due(call,&due);
    if (status!=XR_XIR_CALL_READY || !due) return status;
    bool won=call->channel_watch ? xr_xir_channel_watch_cancel_pending(call->channel_watch) :
        call->channel_send ? xr_xir_channel_send_wait_cancel_pending(call->channel_send) :
        call->channel_recv ? xr_xir_channel_recv_wait_cancel_pending(call->channel_recv) : false;
    if (won) call->channel_timing.expired=true;
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_call_channel_deadline_poll(XrXirCall *call,const XrXirExecutorBinding *binding) {
    if (!call || !binding || !call_owner_current(call)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call)) return XR_XIR_CALL_BUSY;
    if (!xr_xir_call_executor_member(call,binding) || !xr_xir_call_channel_waiting(call)) return XR_XIR_CALL_BAD_STATE;
    return channel_timing_poll(call);
}
