/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_blocking_call_cases.h - Real shared blocking state and recovery
 */
#ifndef XIR_CHANNEL_BLOCKING_CALL_CASES_H
#define XIR_CHANNEL_BLOCKING_CALL_CASES_H
typedef struct BlockingChannelFrame { XrXirChannelCallState operation;XrXirValue argument,result;bool initialized; } BlockingChannelFrame;
typedef struct BlockingChannelWitness { XrXirChannelCallOperation operation;unsigned callbacks,releases;bool unit,bad_resume;XrXirCallStatus reason; } BlockingChannelWitness;
static XrXirAction blocking_channel_entry(XrXirCallView *view) {
    BlockingChannelFrame *frame=view->state;BlockingChannelWitness *w=(BlockingChannelWitness *)view->environment;++w->callbacks;
    if (!frame->initialized) { frame->argument=w->unit ? (XrXirValue){0} : result_integer(w->operation==XR_XIR_CHANNEL_CALL_SEND ? 913 : -7);frame->initialized=true; }
    XrXirType type=w->operation==XR_XIR_CHANNEL_CALL_RECV ? (XrXirType)RESULT_I64 : w->unit ? XR_XIR_UNIT : XR_XIR_I64;
    const XrXirValue *argument=w->operation==XR_XIR_CHANNEL_CALL_RECV ? NULL : &frame->argument;
    const XrXirChannelCallRequest request={w->operation,&view->arguments[0],argument,type,0};
    XrXirCallView copied=*view;size_t before=runtime_memory.attempts;
    CHECK(xr_xir_call_channel_step(&copied,&frame->operation,&request,&frame->result)==XR_XIR_CALL_BAD_STATE);
    CHECK(runtime_memory.attempts==before && unit_value(&frame->result));
    if (w->bad_resume && frame->operation.waiting) {
        XrXirChannelCallState original=frame->operation;XrXirValue inbox=view->inbox.value;
        XrXirChannelCallOperation wrong=w->operation==XR_XIR_CHANNEL_CALL_SEND ? XR_XIR_CHANNEL_CALL_RECV_OR : XR_XIR_CHANNEL_CALL_SEND;
        const XrXirChannelCallRequest wrong_request={wrong,&view->arguments[0],&frame->argument,XR_XIR_I64,0};
        XrXirCallStatus bad=xr_xir_call_channel_step(view,&frame->operation,&wrong_request,&frame->result);
        CHECK(bad==XR_XIR_CALL_BAD_STATE && !memcmp(&original,&frame->operation,sizeof(original)) &&
            !memcmp(&inbox,&view->inbox.value,sizeof(inbox)) && unit_value(&frame->result));w->bad_resume=false;
    }
    XrXirCallStatus status=xr_xir_call_channel_step(view,&frame->operation,&request,&frame->result);
    if (status==XR_XIR_CALL_SUSPENDED) return channel_call_action(XR_XIR_ACTION_AWAIT_CHANNEL,(XrXirValue){0});
    if (status==XR_XIR_CALL_BUSY) return channel_call_action(XR_XIR_ACTION_CONTINUE,(XrXirValue){0});
    if (status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT) return recv_wait_fault(status==XR_XIR_CALL_OOM ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_LIMIT);
    CHECK(status==XR_XIR_CALL_READY && !frame->operation.waiting);int64_t result=frame->result.payload;
    if (w->operation==XR_XIR_CHANNEL_CALL_RECV) {
        uint32_t variant=UINT32_MAX;CHECK(xr_xir_enum_variant(&frame->result,&variant)==XR_XIR_VALUE_OK && (variant==0 || variant==3));result=-3;
        if (!variant) { XrXirEnumBorrow borrow={0};CHECK(xr_xir_enum_borrow(&frame->result,&borrow)==XR_XIR_VALUE_OK && borrow.field_count==1);result=borrow.fields[0].payload; }
    } else if (w->unit) { CHECK(unit_value(&frame->result));result=1; }
    return channel_call_action(XR_XIR_ACTION_RETURN,result_integer(result));
}
static void blocking_channel_release(XrXirCallView *view,XrXirCallStatus reason) {
    BlockingChannelFrame *frame=view->state;BlockingChannelWitness *w=(BlockingChannelWitness *)view->environment;
    xr_xir_value_drop(&frame->argument);xr_xir_value_drop(&frame->result);++w->releases;w->reason=reason;
}
static XrXirCallStatus blocking_channel_prepare(ResultOwner *owner,const XrXirValue *channel,BlockingChannelWitness *w,
    XrXirCallAccounting *accounting,XrXirCall **output) {
    XrXirType parameter=(XrXirType)(w->unit ? RESULT_CHANNEL_UNIT : RESULT_CHANNEL_I64);
    XrXirCallEntry entry={XR_XIR_CALL_ABI_VERSION,&parameter,1,XR_XIR_I64,sizeof(BlockingChannelFrame),blocking_channel_entry,blocking_channel_release,w,0,0};
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=accounting;config.admission=owner->admission;
    return xr_xir_call_new(&config,0,channel,1,output);
}
static void blocking_channel_host_send(ResultOwner *owner,const XrXirValue *channel,bool unit,int64_t number) {
    XrXirValue message=unit ? (XrXirValue){0} : result_integer(number);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_send_owned(channel,&message,&owner->admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT && unit_value(&message));
}
static int64_t blocking_channel_host_recv(ResultOwner *owner,const XrXirValue *channel,bool unit) {
    XrXirValue output={0};XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_receive(channel,&owner->admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    CHECK(unit ? unit_value(&output) : output.type==XR_XIR_I64);int64_t number=unit ? 1 : output.payload;xr_xir_value_drop(&output);return number;
}
static void blocking_channel_matrix(XrXirChannelCallOperation operation,unsigned capacity,bool closed,bool immediate,bool unit) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)(unit ? RESULT_CHANNEL_UNIT : RESULT_CHANNEL_I64),capacity,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    if (capacity && ((operation==XR_XIR_CHANNEL_CALL_SEND && !immediate) || (operation!=XR_XIR_CHANNEL_CALL_SEND && immediate)))
        blocking_channel_host_send(&owner,&channel,unit,81);
    BlockingChannelWitness witness={.operation=operation,.unit=unit,.bad_resume=!immediate};XrXirCallAccounting accounting={0};XrXirCall *call=NULL;
    CHECK(blocking_channel_prepare(&owner,&channel,&witness,&accounting,&call)==XR_XIR_CALL_READY);
    XrXirCallResult result=xr_xir_call_poll_bounded(call,8);
    if (!immediate) {
        CHECK(result.status==XR_XIR_CALL_SUSPENDED && witness.callbacks==1);
        CHECK(capacity ? call->channel_watch!=NULL : operation==XR_XIR_CHANNEL_CALL_SEND ? call->channel_send!=NULL : call->channel_recv!=NULL);
        size_t attempts=runtime_memory.attempts;for (unsigned i=0;i<64;++i) CHECK(xr_xir_call_poll_bounded(call,1).status==XR_XIR_CALL_SUSPENDED);
        CHECK(witness.callbacks==1 && runtime_memory.attempts==attempts);
        if (closed) { XrXirChannelState state=XR_XIR_CHANNEL_RETRY;CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK); }
        else if (operation==XR_XIR_CHANNEL_CALL_SEND) CHECK(blocking_channel_host_recv(&owner,&channel,unit)==(capacity ? 81 : 913));
        else blocking_channel_host_send(&owner,&channel,unit,81);
        attempts=runtime_memory.attempts;result=xr_xir_call_poll_bounded(call,16);
        CHECK(runtime_memory.attempts==attempts+(capacity && operation==XR_XIR_CHANNEL_CALL_RECV && !closed ? 1u : 0u));
    }
    int64_t expected=unit ? 1 : operation==XR_XIR_CHANNEL_CALL_SEND ? closed ? XR_XIR_CHANNEL_SEND_WAIT_CLOSED : XR_XIR_CHANNEL_SEND_WAIT_SENT :
        closed ? operation==XR_XIR_CHANNEL_CALL_RECV ? -3 : -7 : 81;
    CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.payload==expected && witness.releases==1);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && !accounting.live_bytes && accounting.allocations==accounting.frees);
    if (capacity && operation==XR_XIR_CHANNEL_CALL_SEND && !closed) CHECK(blocking_channel_host_recv(&owner,&channel,unit)==913);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void blocking_channel_cancel(XrXirChannelCallOperation operation,unsigned capacity,bool last) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,capacity,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    if (capacity && operation==XR_XIR_CHANNEL_CALL_SEND) blocking_channel_host_send(&owner,&channel,false,81);
    BlockingChannelWitness witness={.operation=operation};XrXirCallAccounting accounting={0};XrXirCall *call=NULL;
    CHECK(blocking_channel_prepare(&owner,&channel,&witness,&accounting,&call)==XR_XIR_CALL_READY && xr_xir_call_poll_bounded(call,8).status==XR_XIR_CALL_SUSPENDED);
    XirChannel *storage=(XirChannel *)object_pointer(&channel);atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    if (last) xr_xir_value_drop(&channel);size_t attempts=runtime_memory.attempts;
    CHECK(xr_xir_call_request_cancel(call)==XR_XIR_CALL_CANCEL_REQUESTED && xr_xir_call_poll_bounded(call,16).status==XR_XIR_CALL_CANCELLED);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && witness.releases==1 && witness.reason==XR_XIR_CALL_CANCELLED && runtime_memory.attempts==attempts);
    if (!last) {
        atomic_store_explicit(&storage->locked,false,memory_order_release);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
        CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK);xr_xir_value_drop(&channel);
    }
    result_owner_free(&owner);
}
static void blocking_channel_drain(ResultOwner *owner,const XrXirValue *channel) {
    for (;;) {
        XrXirValue output={0};XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
        CHECK(xr_xir_channel_receive(channel,&owner->admission,&output,&state)==XR_XIR_VALUE_OK &&
            (state==XR_XIR_CHANNEL_VALUE || state==XR_XIR_CHANNEL_EMPTY));xr_xir_value_drop(&output);
        if (state==XR_XIR_CHANNEL_EMPTY) return;
    }
}
static XrXirCallStatus blocking_channel_fault_operation(ResultOwner *owner,const XrXirValue *channel,
    XrXirChannelCallOperation operation,unsigned capacity) {
    if (capacity && operation==XR_XIR_CHANNEL_CALL_SEND) blocking_channel_host_send(owner,channel,false,81);
    BlockingChannelWitness witness={.operation=operation};XrXirCallAccounting accounting={0};XrXirCall *call=NULL;
    XrXirCallStatus status=blocking_channel_prepare(owner,channel,&witness,&accounting,&call);
    if (status==XR_XIR_CALL_READY) {
        status=xr_xir_call_poll_bounded(call,8).status;
        if (status==XR_XIR_CALL_SUSPENDED) {
            if (operation==XR_XIR_CHANNEL_CALL_SEND) CHECK(blocking_channel_host_recv(owner,channel,false)==(capacity ? 81 : 913));
            else blocking_channel_host_send(owner,channel,false,81);
            XrXirCallResult result=xr_xir_call_poll_bounded(call,16);status=result.status;
            if (status==XR_XIR_CALL_RETURNED) CHECK(result.value.payload==(operation==XR_XIR_CHANNEL_CALL_SEND ? XR_XIR_CHANNEL_SEND_WAIT_SENT : 81));
        }
    }
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && !accounting.live_bytes && accounting.allocations==accounting.frees);
    blocking_channel_drain(owner,channel);XirChannel *storage=(XirChannel *)object_pointer(channel);
    CHECK(!storage->buffer.count && !storage->send_count && !storage->recv_count && !storage->watch_count);return status;
}
static void blocking_channel_faults(XrXirChannelCallOperation operation,unsigned capacity) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,capacity,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    size_t sites=0,live=runtime_memory.live,bytes=runtime_memory.bytes;
    for (size_t ordinal=0;ordinal<=sites;++ordinal) {
        size_t begin=runtime_memory.attempts;runtime_memory.fail_at=ordinal ? begin+ordinal-1 : SIZE_MAX;
        XrXirCallStatus status=blocking_channel_fault_operation(&owner,&channel,operation,capacity);size_t attempts=runtime_memory.attempts-begin;runtime_memory.fail_at=SIZE_MAX;
        if (!ordinal) { CHECK(status==XR_XIR_CALL_RETURNED);sites=attempts;CHECK(sites); }
        else CHECK(status==XR_XIR_CALL_OOM && attempts>=ordinal);
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
        CHECK(blocking_channel_fault_operation(&owner,&channel,operation,capacity)==XR_XIR_CALL_RETURNED);
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
    }
    printf("Shared blocking Call operation=%u capacity=%u complete runtime allocator ordinals=%zu; same-Channel/Domain recovery physical baseline\n",operation,capacity,sites);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void blocking_channel_cases(void) {
    for (unsigned op=XR_XIR_CHANNEL_CALL_SEND;op<=XR_XIR_CHANNEL_CALL_RECV_OR;++op) for (unsigned capacity=0;capacity<2;++capacity) {
        for (unsigned closed=0;closed<2;++closed) blocking_channel_matrix((XrXirChannelCallOperation)op,capacity,closed!=0,false,false);
        for (unsigned last=0;last<2;++last) blocking_channel_cancel((XrXirChannelCallOperation)op,capacity,last!=0);
        blocking_channel_faults((XrXirChannelCallOperation)op,capacity);
        if (capacity) blocking_channel_matrix((XrXirChannelCallOperation)op,capacity,false,true,false);
    }
    for (unsigned capacity=0;capacity<2;++capacity) for (unsigned closed=0;closed<2;++closed)
        blocking_channel_matrix(XR_XIR_CHANNEL_CALL_RECV_OR,capacity,closed!=0,false,true);
    puts("Shared blocking Call state machine: zero/buffered send/recv/recvOr, actual peer delivery or close, authenticated continuation,64 pending polls, held-lock cancellation and complete execution FI physical0");
}
#endif // XIR_CHANNEL_BLOCKING_CALL_CASES_H
