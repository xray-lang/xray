/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_inbox_move_cases.h - Exact active-view result ownership transfer
 */
#ifndef XIR_CHANNEL_INBOX_MOVE_CASES_H
#define XIR_CHANNEL_INBOX_MOVE_CASES_H
static XrXirAction channel_inbox_move_entry(XrXirCallView *view) {
    RecvWaitFrame *frame=view->state;RendezvousWitness *w=(RendezvousWitness *)view->environment;++w->callbacks;
    if (!frame->started) {
        XrXirChannelRecvWaitState state=XR_XIR_CHANNEL_RECV_WAIT_RETRY;XrXirValueStatus status;
        if (!w->mode) status=xr_xir_channel_rendezvous_recv_begin(&view->arguments[0],(XrXirType)RESULT_I64,xr_xir_call_admission(view),&frame->recv,&state);
        else status=xr_xir_channel_rendezvous_recv_raw_begin(&view->arguments[0],xr_xir_call_admission(view),&frame->recv,&state);
        if (status!=XR_XIR_VALUE_OK) return recv_wait_fault(status);
        CHECK(state==XR_XIR_CHANNEL_RECV_WAIT_PENDING && xr_xir_call_wait_channel_recv(view,&frame->recv)==XR_XIR_CALL_SUSPENDED && !frame->recv);
        frame->started=true;return channel_call_action(XR_XIR_ACTION_AWAIT_CHANNEL,(XrXirValue){0});
    }
    XrXirType expected=!w->mode ? (XrXirType)RESULT_I64 : w->mode==1 ? XR_XIR_I64 : XR_XIR_UNIT;
    XrXirCallResult before=view->inbox;CHECK(before.status==XR_XIR_CALL_RETURNED);XirObject *object=!w->mode ? object_pointer(&before.value) : NULL;
    uint32_t refs=object ? atomic_load_explicit(&object->references,memory_order_relaxed) : 0;
    size_t attempts=runtime_memory.attempts;runtime_memory.fail_at=attempts;XrXirCallView copied=*view;
    CHECK(xr_xir_call_take_inbox(&copied,expected,&frame->result)==XR_XIR_CALL_BAD_STATE && unit_value(&frame->result));
    XrXirType wrong=!w->mode ? XR_XIR_I64 : XR_XIR_STRING;
    CHECK(xr_xir_call_take_inbox(view,wrong,&frame->result)==XR_XIR_CALL_BAD_STATE && unit_value(&frame->result));
    XrXirValue occupied=result_integer(99);CHECK(xr_xir_call_take_inbox(view,expected,&occupied)==XR_XIR_CALL_BAD_ARGUMENT && occupied.payload==99);
    CHECK(before.status==view->inbox.status && before.wake==view->inbox.wake &&
        !memcmp(&before.value,&view->inbox.value,sizeof(before.value)) && xr_xir_panic_empty(&view->inbox.panic));
    CHECK(xr_xir_call_take_inbox(view,expected,&frame->result)==XR_XIR_CALL_READY && xr_xir_call_result_empty(&view->inbox));
    CHECK(!memcmp(&before.value,&frame->result,sizeof(frame->result)));
    if (object) CHECK(atomic_load_explicit(&object->references,memory_order_relaxed)==refs);
    XrXirValue again={0};CHECK(xr_xir_call_take_inbox(view,expected,&again)==XR_XIR_CALL_BAD_STATE && unit_value(&again));
    CHECK(xr_xir_call_discard_inbox(view,expected)==XR_XIR_CALL_BAD_STATE && runtime_memory.attempts==attempts);runtime_memory.fail_at=SIZE_MAX;
    int64_t value=1;
    if (!w->mode) { XrXirEnumBorrow borrow={0};CHECK(xr_xir_enum_borrow(&frame->result,&borrow)==XR_XIR_VALUE_OK && borrow.field_count==1);value=borrow.fields[0].payload; }
    else if (w->mode==1) value=frame->result.payload;
    else CHECK(unit_value(&frame->result));
    return channel_call_action(XR_XIR_ACTION_RETURN,result_integer(value));
}
static void channel_inbox_move(unsigned mode) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=mode==2 ? (XrXirValue){0} : result_integer(823);
    XrXirType parameter=(XrXirType)(mode==2 ? RESULT_CHANNEL_UNIT : RESULT_CHANNEL_I64);
    CHECK(xr_xir_channel_new(parameter,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    RendezvousWitness witness={.mode=mode};XrXirCallAccounting accounting={0};XrXirCall *call=NULL;
    XrXirCallEntry entry={XR_XIR_CALL_ABI_VERSION,&parameter,1,XR_XIR_I64,sizeof(RecvWaitFrame),channel_inbox_move_entry,recv_wait_call_release,&witness,0,0};
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=&accounting;config.admission=owner.admission;
    CHECK(xr_xir_call_new(&config,0,&channel,1,&call)==XR_XIR_CALL_READY && xr_xir_call_poll_bounded(call,1).status==XR_XIR_CALL_SUSPENDED);
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;CHECK(xr_xir_channel_send_owned(&channel,&message,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT);
    XrXirCallResult result=xr_xir_call_poll_bounded(call,8);CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.payload==(mode==2 ? 1 : 823));
    CHECK(witness.releases==1 && xr_xir_call_free(call)==XR_XIR_CALL_READY && !accounting.live_bytes);
    xr_xir_value_drop(&message);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void channel_raw_array_faults(void) {
    size_t sites=0;
    for (size_t ordinal=0;ordinal<=sites;++ordinal) {
        ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message={0},output={0};
        rendezvous_fault_message(&owner,true,&channel,&message);XrXirValue original=message;
        XrXirChannelRecvWait *recv=recv_raw_begin(&owner,&channel);XirChannel *storage=(XirChannel *)object_pointer(&channel);
        XrXirChannelWatch *watch=NULL;XrXirChannelWatchState observed=XR_XIR_CHANNEL_WATCH_RETRY;
        CHECK(xr_xir_channel_watch_begin(&channel,channel_call_sequence(&owner,&channel),&owner.admission,&watch,&observed)==XR_XIR_VALUE_OK && watch);
        size_t live=runtime_memory.live,bytes=runtime_memory.bytes,begin=runtime_memory.attempts;uint64_t sequence=storage->sequence;
        runtime_memory.fail_at=ordinal ? begin+ordinal-1 : SIZE_MAX;XrXirChannelState state=XR_XIR_CHANNEL_EMPTY;
        XrXirValueStatus status=xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state);
        size_t attempts=runtime_memory.attempts-begin;runtime_memory.fail_at=SIZE_MAX;
        if (!ordinal) { CHECK(status==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT);sites=attempts;CHECK(sites==1); }
        else {
            CHECK(status==XR_XIR_VALUE_OOM && attempts>=ordinal && unit_value(&output) && state==XR_XIR_CHANNEL_EMPTY);
            CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes && storage->sequence==sequence && storage->recv_count==1 &&
                xr_xir_channel_recv_wait_state(recv)==XR_XIR_CHANNEL_RECV_WAIT_PENDING && !xr_xir_channel_watch_ready(watch));
            CHECK(recv->raw && !recv->reservation && !recv->taken && unit_value(&recv->result));
            CHECK(xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT);
        }
        CHECK(!memcmp(&message,&original,sizeof(message)) && xr_xir_channel_watch_ready(watch));xr_xir_value_drop(&output);
        CHECK(xr_xir_channel_recv_wait_take(recv,&output) && output.type==RESULT_ARRAY);
        XrXirValue first={0};XrXirFaultDetail fault={0};CHECK(xr_xir_array_get(&output,0,&owner.admission,&first,&fault)==XR_XIR_VALUE_OK && first.payload==17);
        xr_xir_value_drop(&first);xr_xir_value_drop(&output);xr_xir_channel_recv_wait_drop(recv);xr_xir_channel_watch_drop(watch);
        xr_xir_value_drop(&message);xr_xir_value_drop(&channel);result_owner_free(&owner);
    }
    printf("Bare Array trySend complete runtime allocator ordinals=%zu; unchanged raw owner/watch before transfer and same-node recovery physical0\n",sites);
}
static void channel_inbox_move_cases(void) {
    for (unsigned mode=0;mode<3;++mode) channel_inbox_move(mode);
    channel_raw_array_faults();
    puts("Call actual inbox move: native Recv/i64/Unit, no allocation or retain, exact active view/type, failed outputs preserved, take once and physical0");
}
#endif // XIR_CHANNEL_INBOX_MOVE_CASES_H
