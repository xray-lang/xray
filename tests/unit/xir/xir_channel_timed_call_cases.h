/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_timed_call_cases.h - Real monotonic deadlines and owned terminal results
 */
#ifndef XIR_CHANNEL_TIMED_CALL_CASES_H
#define XIR_CHANNEL_TIMED_CALL_CASES_H
#include "execution/xr_xir_host_time.h"
#include "os/os_time.h"
typedef struct TimedChannelFrame {
    XrXirChannelCallState operation;
    XrXirValue argument,result;
    bool initialized;
} TimedChannelFrame;
typedef struct TimedChannelWitness {
    XrXirChannelCallOperation operation;
    XrXirTimeProvider provider,system;
    XrXirCall *call;
    int64_t milliseconds,first;
    unsigned callbacks,releases,reads,bad_clock;
    bool unit,reenter,bad_resume;
    XrXirCallStatus reason;
} TimedChannelWitness;
static XrXirTimeStatus timed_channel_clock(void *context,XrXirClockKind clock,int64_t *nanoseconds) {
    TimedChannelWitness *w=context;++w->reads;
    CHECK(clock==XR_XIR_CLOCK_MONOTONIC);
    if (w->reenter) {
        CHECK(w->call && xr_xir_call_request_cancel(w->call)==XR_XIR_CALL_BUSY);
        CHECK(xr_xir_call_poll_bounded(w->call,1).status==XR_XIR_CALL_BUSY && xr_xir_call_free(w->call)==XR_XIR_CALL_BUSY);
    }
    if (w->bad_clock==1) return XR_XIR_TIME_FAILED;
    if (w->bad_clock==2) { *nanoseconds=0;return XR_XIR_TIME_OK; }
    if (w->bad_clock==3 && w->reads>1) { *nanoseconds=w->first-1;return XR_XIR_TIME_OK; }
    XrXirTimeStatus status=w->system.clock(w->system.context,clock,nanoseconds);
    if (status==XR_XIR_TIME_OK && !w->first) w->first=*nanoseconds;
    return status;
}
static void timed_channel_provider(TimedChannelWitness *w) {
    xr_xir_host_time_provider(&w->system);w->provider=w->system;
    w->provider.clock=timed_channel_clock;w->provider.context=w;
}
static XrXirAction timed_channel_entry(XrXirCallView *view) {
    TimedChannelFrame *frame=view->state;TimedChannelWitness *w=(TimedChannelWitness *)view->environment;++w->callbacks;
    if (!frame->initialized) { frame->argument=w->unit ? (XrXirValue){0} : result_integer(913);frame->initialized=true; }
    bool sending=w->operation==XR_XIR_CHANNEL_CALL_SEND_TIMEOUT;
    XrXirType type=(XrXirType)(sending ? RESULT_SEND : w->unit ? RESULT_UNIT : RESULT_I64);
    XrXirChannelCallRequest request={w->operation,&view->arguments[0],sending ? &frame->argument : NULL,type,w->milliseconds};
    if (w->bad_resume && frame->operation.waiting) {
        XrXirChannelCallState before=frame->operation;XrXirValue inbox=view->inbox.value;++request.timeout_ms;
        CHECK(xr_xir_call_channel_step(view,&frame->operation,&request,&frame->result)==XR_XIR_CALL_BAD_STATE);
        CHECK(!memcmp(&before,&frame->operation,sizeof(before)) && !memcmp(&inbox,&view->inbox.value,sizeof(inbox)) && unit_value(&frame->result));
        --request.timeout_ms;w->bad_resume=false;
    }
    XrXirCallStatus status=xr_xir_call_channel_step(view,&frame->operation,&request,&frame->result);
    if (status==XR_XIR_CALL_SUSPENDED) return channel_call_action(XR_XIR_ACTION_AWAIT_CHANNEL,(XrXirValue){0});
    if (status==XR_XIR_CALL_BUSY) return channel_call_action(XR_XIR_ACTION_CONTINUE,(XrXirValue){0});
    if (status!=XR_XIR_CALL_READY) return xr_xir_call_fault(status==XR_XIR_CALL_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :
        status==XR_XIR_CALL_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : status==XR_XIR_CALL_HOST_ERROR ? XR_XIR_RUN_HOST_ERROR :
        status==XR_XIR_CALL_NUMERIC_RANGE ? XR_XIR_RUN_NUMERIC_RANGE : XR_XIR_RUN_BAD_ARTIFACT);
    CHECK(!frame->operation.waiting);
    return channel_call_action(XR_XIR_ACTION_RETURN,frame->result);
}
static void timed_channel_release(XrXirCallView *view,XrXirCallStatus reason) {
    TimedChannelFrame *frame=view->state;TimedChannelWitness *w=(TimedChannelWitness *)view->environment;
    xr_xir_value_drop(&frame->argument);xr_xir_value_drop(&frame->result);++w->releases;w->reason=reason;
}
static XrXirCallStatus timed_channel_prepare(ResultOwner *owner,const XrXirValue *channel,TimedChannelWitness *w,
    XrXirCallAccounting *accounting) {
    XrXirType parameter=(XrXirType)(w->unit ? RESULT_CHANNEL_UNIT : RESULT_CHANNEL_I64);
    XrXirType result=(XrXirType)(w->operation==XR_XIR_CHANNEL_CALL_SEND_TIMEOUT ? RESULT_SEND : w->unit ? RESULT_UNIT : RESULT_I64);
    XrXirCallEntry entry={XR_XIR_CALL_ABI_VERSION,&parameter,1,result,sizeof(TimedChannelFrame),timed_channel_entry,timed_channel_release,w,0,0};
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=accounting;config.admission=owner->admission;config.time=w->provider;
    return xr_xir_call_new(&config,0,channel,1,&w->call);
}
static void timed_channel_free(TimedChannelWitness *w,XrXirCallAccounting *accounting) {
    CHECK(xr_xir_call_free(w->call)==XR_XIR_CALL_READY);w->call=NULL;
    CHECK(!accounting->live_bytes && accounting->allocations==accounting->frees);
}
static void timed_channel_close(ResultOwner *owner,const XrXirValue *channel) {
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_close(channel,&owner->admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_CLOSED);
}
static void timed_channel_check(XrXirCallResult result,uint32_t variant,bool unit) {
    CHECK(result.status==XR_XIR_CALL_RETURNED);result_variant(&result.value,variant,variant==0 && result.value.type!=RESULT_SEND ? 1u : 0u);
    if (!variant && result.value.type!=RESULT_SEND) {
        XrXirEnumBorrow borrow={0};CHECK(xr_xir_enum_borrow(&result.value,&borrow)==XR_XIR_VALUE_OK);
        CHECK(unit ? unit_value(&borrow.fields[0]) : borrow.fields[0].payload==73);
    }
}
static void timed_channel_zero(XrXirChannelCallOperation operation,unsigned capacity,bool unit,unsigned outcome) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)(unit ? RESULT_CHANNEL_UNIT : RESULT_CHANNEL_I64),capacity,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    bool sending=operation==XR_XIR_CHANNEL_CALL_SEND_TIMEOUT;
    if (capacity && (sending ? outcome==2 : outcome==0)) blocking_channel_host_send(&owner,&channel,unit,73);
    if (outcome==3) timed_channel_close(&owner,&channel);
    TimedChannelWitness w={.operation=operation,.unit=unit};XrXirCallAccounting accounting={0};
    CHECK(timed_channel_prepare(&owner,&channel,&w,&accounting)==XR_XIR_CALL_READY);
    timed_channel_check(xr_xir_call_poll_bounded(w.call,16),capacity || outcome==3 ? outcome : 2,unit);
    CHECK(w.callbacks==1 && !w.reads && w.releases==1);timed_channel_free(&w,&accounting);
    if (capacity && sending && outcome!=3) CHECK(blocking_channel_host_recv(&owner,&channel,unit)==(unit ? 1 : outcome==0 ? 913 : 73));
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void timed_channel_wait(XrXirChannelCallOperation operation,unsigned capacity,bool unit,unsigned winner) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)(unit ? RESULT_CHANNEL_UNIT : RESULT_CHANNEL_I64),capacity,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    bool sending=operation==XR_XIR_CHANNEL_CALL_SEND_TIMEOUT;
    if (capacity && sending) blocking_channel_host_send(&owner,&channel,unit,81);
    TimedChannelWitness w={.operation=operation,.unit=unit,.milliseconds=winner==2 ? 10 : 5000,.bad_resume=true,.reenter=true};
    timed_channel_provider(&w);XrXirCallAccounting accounting={0};
    CHECK(timed_channel_prepare(&owner,&channel,&w,&accounting)==XR_XIR_CALL_READY);
    XrXirCallResult result=xr_xir_call_poll_bounded(w.call,8);CHECK(result.status==XR_XIR_CALL_SUSPENDED && w.callbacks==1);
    uint64_t start=w.call->channel_timing.start_ns;size_t attempts=runtime_memory.attempts;
    if (winner!=2) {
        for (unsigned i=0;i<64;++i) CHECK(xr_xir_call_poll_bounded(w.call,1).status==XR_XIR_CALL_SUSPENDED);
        CHECK(w.callbacks==1 && runtime_memory.attempts==attempts && w.call->channel_timing.start_ns==start);
        CHECK(xr_xir_call_resume(w.call,result.wake)==XR_XIR_CALL_BAD_STATE);
        if (winner==3) timed_channel_close(&owner,&channel);
        else if (sending) CHECK(blocking_channel_host_recv(&owner,&channel,unit)==(unit ? 1 : capacity ? 81 : 913));
        else blocking_channel_host_send(&owner,&channel,unit,73);
    }
    do {
        if (winner==2) xr_time_sleep_ms(1);
        result=xr_xir_call_poll_bounded(w.call,16);
    } while (result.status==XR_XIR_CALL_SUSPENDED || result.status==XR_XIR_CALL_READY);
    timed_channel_check(result,winner,unit);
    CHECK(w.releases==1 && w.callbacks==2 && w.reads>=2);
    if (winner==2) CHECK(xr_time_monotonic_ns()-start>=UINT64_C(10000000));
    timed_channel_free(&w,&accounting);
    if (capacity && sending && winner!=3) CHECK(blocking_channel_host_recv(&owner,&channel,unit)==(unit ? 1 : winner==2 ? 81 : 913));
    timed_channel_close(&owner,&channel);XirChannel *storage=(XirChannel *)object_pointer(&channel);
    CHECK(!storage->send_count && !storage->recv_count && !storage->watch_count);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void timed_channel_clock_failure(unsigned mode,bool parked) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    TimedChannelWitness w={.operation=XR_XIR_CHANNEL_CALL_RECV_TIMEOUT,.milliseconds=5000};
    if (mode) timed_channel_provider(&w);
    if (!parked) w.bad_clock=mode;
    XrXirCallAccounting accounting={0};CHECK(timed_channel_prepare(&owner,&channel,&w,&accounting)==XR_XIR_CALL_READY);
    XrXirCallResult result=xr_xir_call_poll_bounded(w.call,8);
    if (parked) { CHECK(result.status==XR_XIR_CALL_SUSPENDED);w.bad_clock=mode;result=xr_xir_call_poll_bounded(w.call,16); }
    CHECK(result.status==XR_XIR_CALL_HOST_ERROR && w.releases==1 && !w.call->channel_timing.active);
    timed_channel_free(&w,&accounting);timed_channel_close(&owner,&channel);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void timed_channel_range(int64_t milliseconds) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    TimedChannelWitness w={.operation=XR_XIR_CHANNEL_CALL_SEND_TIMEOUT,.milliseconds=milliseconds};
    XrXirCallAccounting accounting={0};CHECK(timed_channel_prepare(&owner,&channel,&w,&accounting)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll_bounded(w.call,16).status==XR_XIR_CALL_NUMERIC_RANGE && !w.reads);
    CHECK(!((XirChannel *)object_pointer(&channel))->buffer.count);timed_channel_free(&w,&accounting);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void timed_channel_late_poll(XrXirChannelCallOperation operation,bool closed) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    TimedChannelWitness w={.operation=operation,.milliseconds=1000};timed_channel_provider(&w);
    XrXirCallAccounting accounting={0};CHECK(timed_channel_prepare(&owner,&channel,&w,&accounting)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll_bounded(w.call,8).status==XR_XIR_CALL_SUSPENDED);
    uint64_t start=w.call->channel_timing.start_ns;
    if (closed) timed_channel_close(&owner,&channel);
    else if (operation==XR_XIR_CHANNEL_CALL_SEND_TIMEOUT) CHECK(blocking_channel_host_recv(&owner,&channel,false)==913);
    else blocking_channel_host_send(&owner,&channel,false,73);
    unsigned reads=w.reads;w.bad_clock=1;
    while (xr_time_monotonic_ns()-start<UINT64_C(1000000000)) xr_time_sleep_ms(1);
    timed_channel_check(xr_xir_call_poll_bounded(w.call,16),closed ? 3u : 0u,false);
    CHECK(w.reads==reads && w.releases==1);timed_channel_free(&w,&accounting);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void timed_channel_watch_rearm(XrXirChannelCallOperation operation) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    bool sending=operation==XR_XIR_CHANNEL_CALL_SEND_TIMEOUT;
    if (sending) blocking_channel_host_send(&owner,&channel,false,81);
    TimedChannelWitness w={.operation=operation,.milliseconds=1000,.bad_resume=true};timed_channel_provider(&w);
    XrXirCallAccounting accounting={0};CHECK(timed_channel_prepare(&owner,&channel,&w,&accounting)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll_bounded(w.call,8).status==XR_XIR_CALL_SUSPENDED);
    uint64_t start=w.call->channel_timing.start_ns;
    if (sending) CHECK(blocking_channel_host_recv(&owner,&channel,false)==81);
    blocking_channel_host_send(&owner,&channel,false,73);
    if (!sending) CHECK(blocking_channel_host_recv(&owner,&channel,false)==73);
    CHECK(xr_xir_call_poll_bounded(w.call,8).status==XR_XIR_CALL_SUSPENDED && w.callbacks==2);
    CHECK(w.call->channel_timing.start_ns==start && w.call->channel_watch);
    while (xr_time_monotonic_ns()-start<UINT64_C(1000000000)) xr_time_sleep_ms(1);
    timed_channel_check(xr_xir_call_poll_bounded(w.call,16),2,false);
    CHECK(w.callbacks==3 && w.releases==1);timed_channel_free(&w,&accounting);
    if (sending) CHECK(blocking_channel_host_recv(&owner,&channel,false)==73);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void timed_channel_cancel(XrXirChannelCallOperation operation,unsigned capacity) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,capacity,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    if (capacity && operation==XR_XIR_CHANNEL_CALL_SEND_TIMEOUT) blocking_channel_host_send(&owner,&channel,false,81);
    TimedChannelWitness w={.operation=operation,.milliseconds=5000};timed_channel_provider(&w);
    XrXirCallAccounting accounting={0};CHECK(timed_channel_prepare(&owner,&channel,&w,&accounting)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll_bounded(w.call,8).status==XR_XIR_CALL_SUSPENDED);
    XirChannel *storage=(XirChannel *)object_pointer(&channel);atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    size_t attempts=runtime_memory.attempts;unsigned reads=w.reads;
    CHECK(xr_xir_call_request_cancel(w.call)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(xr_xir_call_poll_bounded(w.call,16).status==XR_XIR_CALL_CANCELLED && w.reason==XR_XIR_CALL_CANCELLED && w.releases==1);
    timed_channel_free(&w,&accounting);CHECK(runtime_memory.attempts==attempts && w.reads==reads);
    atomic_store_explicit(&storage->locked,false,memory_order_release);timed_channel_close(&owner,&channel);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void channel_timed_call_cases(void) {
    channel_call_retired_prefix();
    for (unsigned op=XR_XIR_CHANNEL_CALL_SEND_TIMEOUT;op<=XR_XIR_CHANNEL_CALL_RECV_TIMEOUT;++op)
        for (unsigned capacity=0;capacity<2;++capacity) for (unsigned unit=0;unit<2;++unit) {
            for (unsigned outcome=0;outcome<=3;++outcome) if (outcome!=1)
                timed_channel_zero((XrXirChannelCallOperation)op,capacity,unit!=0,outcome);
            for (unsigned winner=0;winner<=3;++winner) if (winner!=1)
                timed_channel_wait((XrXirChannelCallOperation)op,capacity,unit!=0,winner);
        }
    for (unsigned mode=0;mode<=3;++mode) { timed_channel_clock_failure(mode,false);if (mode) timed_channel_clock_failure(mode,true); }
    timed_channel_range(-1);timed_channel_range((int64_t)XR_XIR_TIMER_MAX_MS+1);
    for (unsigned op=XR_XIR_CHANNEL_CALL_SEND_TIMEOUT;op<=XR_XIR_CHANNEL_CALL_RECV_TIMEOUT;++op) {
        timed_channel_late_poll((XrXirChannelCallOperation)op,false);timed_channel_late_poll((XrXirChannelCallOperation)op,true);
        timed_channel_watch_rearm((XrXirChannelCallOperation)op);
        for (unsigned capacity=0;capacity<2;++capacity) timed_channel_cancel((XrXirChannelCallOperation)op,capacity);
    }
    puts("Timed shared Call: real host monotonic Timeout, peer/close first, zero one-attempt, Unit,64 stable pending polls, clock errors/reentry and retired Call32 rejection physical0");
}
#endif // XIR_CHANNEL_TIMED_CALL_CASES_H
