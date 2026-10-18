/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_recv_wait_call_cases.h - Owned native receive inbox and unwind
 */
#ifndef XIR_CHANNEL_RECV_WAIT_CALL_CASES_H
#define XIR_CHANNEL_RECV_WAIT_CALL_CASES_H
typedef struct RecvWaitFrame { XrXirChannelRecvWait *recv;XrXirValue result;bool started; } RecvWaitFrame;
static XrXirAction recv_wait_fault(XrXirValueStatus status) {
    CHECK(status==XR_XIR_VALUE_OOM || status==XR_XIR_VALUE_LIMIT);
    return xr_xir_call_fault(status==XR_XIR_VALUE_OOM ? XR_XIR_RUN_OUT_OF_MEMORY : XR_XIR_RUN_FRAME_LIMIT);
}
static XrXirAction recv_wait_entry(XrXirCallView *view) {
    RecvWaitFrame *frame=view->state;RendezvousWitness *w=(RendezvousWitness *)view->environment;++w->callbacks;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);CHECK(admission && view->argument_count==1);
    if (!frame->started) {
        XrXirChannelRecvWaitState state=XR_XIR_CHANNEL_RECV_WAIT_RETRY;
        XrXirValueStatus status=xr_xir_channel_rendezvous_recv_begin(&view->arguments[0],(XrXirType)RESULT_I64,admission,&frame->recv,&state);
        if (status!=XR_XIR_VALUE_OK) return recv_wait_fault(status);
        CHECK(frame->recv && state==XR_XIR_CHANNEL_RECV_WAIT_PENDING);frame->started=true;
    }
    const XrXirValue *result=&view->inbox.value;
    if (frame->recv) {
        XrXirCallView copied=*view;size_t attempts=runtime_memory.attempts;
        CHECK(xr_xir_call_wait_channel_recv(&copied,&frame->recv)==XR_XIR_CALL_BAD_STATE);
        XrXirCallStatus status=xr_xir_call_wait_channel_recv(view,&frame->recv);CHECK(runtime_memory.attempts==attempts);
        if (status==XR_XIR_CALL_SUSPENDED) {
            CHECK(!frame->recv);
            if (w->mode==1) return channel_call_action(XR_XIR_ACTION_CONTINUE,(XrXirValue){0});
            if (w->mode==2) CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_CANCEL_REQUESTED);
            return channel_call_action(XR_XIR_ACTION_AWAIT_CHANNEL,(XrXirValue){0});
        }
        CHECK(status==XR_XIR_CALL_READY && xr_xir_channel_recv_wait_take(frame->recv,&frame->result));result=&frame->result;
    } else CHECK(view->inbox.status==XR_XIR_CALL_RETURNED);
    uint32_t variant=UINT32_MAX;CHECK(xr_xir_enum_variant(result,&variant)==XR_XIR_VALUE_OK && (variant==0 || variant==3));
    int64_t number=-3;
    if (!variant) { XrXirEnumBorrow borrow={0};CHECK(xr_xir_enum_borrow(result,&borrow)==XR_XIR_VALUE_OK && borrow.field_count==1);number=borrow.fields[0].payload; }
    return channel_call_action(XR_XIR_ACTION_RETURN,result_integer(number));
}
static XrXirAction recv_wait_sender_entry(XrXirCallView *view) {
    RecvWaitFrame *frame=view->state;RendezvousWitness *w=(RendezvousWitness *)view->environment;++w->callbacks;
    XrXirValue message=result_integer(701);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    XrXirValueStatus status=xr_xir_channel_try_send(&view->arguments[0],&message,(XrXirType)RESULT_SEND,xr_xir_call_admission(view),&frame->result,&state);
    if (status!=XR_XIR_VALUE_OK) return recv_wait_fault(status);
    CHECK(state==XR_XIR_CHANNEL_SENT && message.payload==701);result_variant(&frame->result,0,0);
    return channel_call_action(XR_XIR_ACTION_RETURN,result_integer(1));
}
static void recv_wait_call_release(XrXirCallView *view,XrXirCallStatus reason) {
    RecvWaitFrame *frame=view->state;RendezvousWitness *w=(RendezvousWitness *)view->environment;
    xr_xir_channel_recv_wait_drop(frame->recv);frame->recv=NULL;xr_xir_value_drop(&frame->result);++w->releases;w->reason=reason;
}
static XrXirCallStatus recv_wait_call_prepare(const XrXirValueAdmission *admission,const XrXirValue *channel,bool receiver,
    RendezvousWitness *w,XrXirCallAccounting *accounting,XrXirCall **output) {
    const XrXirType parameter=(XrXirType)RESULT_CHANNEL_I64;
    XrXirCallEntry entry={XR_XIR_CALL_ABI_VERSION,&parameter,1,XR_XIR_I64,sizeof(RecvWaitFrame),
        receiver ? recv_wait_entry : recv_wait_sender_entry,recv_wait_call_release,w,0,0};
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=accounting;config.admission=*admission;
    return xr_xir_call_new(&config,0,channel,1,output);
}
static void recv_wait_call_pair(bool closed,bool cross_domain) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(701),sent={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirValueAdmission receiver_admission=owner.admission;XrXirDomain *peer=NULL;
    if (cross_domain) {
        XrXirDomainBudgetControls controls={8388608,8388608,128000000,4194304,4194304};
        CHECK(xr_xir_domain_new_budgeted(8388608,&controls,&peer)==XR_XIR_VALUE_OK);receiver_admission.domain=peer;
    }
    RendezvousWitness witness={0};XrXirCallAccounting accounting={0};XrXirCall *receiver=NULL;
    CHECK(recv_wait_call_prepare(&receiver_admission,&channel,true,&witness,&accounting,&receiver)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll_bounded(receiver,1).status==XR_XIR_CALL_SUSPENDED && receiver->channel_recv && !receiver->channel_send && !receiver->channel_watch);
    CHECK(!((RecvWaitFrame *)receiver->top->state)->recv);size_t attempts=runtime_memory.attempts;
    XrXirCallResult pending={0};for (unsigned i=0;i<256;++i) { pending=xr_xir_call_poll_bounded(receiver,1);CHECK(pending.status==XR_XIR_CALL_SUSPENDED); }
    CHECK(witness.callbacks==1 && runtime_memory.attempts==attempts && xr_xir_call_resume(receiver,pending.wake)==XR_XIR_CALL_BAD_STATE);
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    if (closed) CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK);
    else CHECK(xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&sent,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT);
    CHECK(xr_xir_call_channel_ready(receiver) && xr_xir_call_resume(receiver,pending.wake)==XR_XIR_CALL_BAD_STATE);
    attempts=runtime_memory.attempts;CHECK(xr_xir_call_poll_bounded(receiver,1).status==XR_XIR_CALL_READY && witness.callbacks==1);
    XrXirCallResult result=xr_xir_call_poll_bounded(receiver,8);CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.payload==(closed ? -3 : 701));
    CHECK(runtime_memory.attempts==attempts && witness.releases==1 && !receiver->channel_recv);
    CHECK(xr_xir_call_free(receiver)==XR_XIR_CALL_READY && !accounting.live_bytes);
    xr_xir_domain_drop(peer);xr_xir_value_drop(&sent);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void recv_wait_call_cancel(bool direct_free,bool last) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    RendezvousWitness witness={0};XrXirCallAccounting accounting={0};XrXirCall *call=NULL;
    CHECK(recv_wait_call_prepare(&owner.admission,&channel,true,&witness,&accounting,&call)==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_poll_bounded(call,1).status==XR_XIR_CALL_SUSPENDED);XirChannel *storage=(XirChannel *)object_pointer(&channel);
    atomic_store_explicit(&storage->locked,true,memory_order_relaxed);if (last) xr_xir_value_drop(&channel);
    size_t attempts=runtime_memory.attempts;
    if (!direct_free) { CHECK(xr_xir_call_request_cancel(call)==XR_XIR_CALL_CANCEL_REQUESTED);CHECK(xr_xir_call_poll_bounded(call,8).status==XR_XIR_CALL_CANCELLED); }
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && witness.releases==1 && witness.reason==XR_XIR_CALL_CANCELLED && runtime_memory.attempts==attempts);
    if (!last) {
        CHECK(storage->recv_count==1 && atomic_load_explicit(&storage->recv_head->state,memory_order_acquire)==CHANNEL_RECV_CANCELLED);
        atomic_store_explicit(&storage->locked,false,memory_order_release);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
        CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK && !storage->recv_count);xr_xir_value_drop(&channel);
    }
    result_owner_free(&owner);
}
static XrXirCallStatus recv_wait_call_fault_operation(ResultOwner *owner,const XrXirValue *channel) {
    RendezvousWitness received={0},sent={0};XrXirCallAccounting ra={0},sa={0};XrXirCall *receiver=NULL,*sender=NULL;
    XrXirCallStatus status=recv_wait_call_prepare(&owner->admission,channel,true,&received,&ra,&receiver);
    if (status==XR_XIR_CALL_READY) {
        status=xr_xir_call_poll_bounded(receiver,1).status;
        if (status==XR_XIR_CALL_READY) status=xr_xir_call_poll_bounded(receiver,8).status;
        if (status==XR_XIR_CALL_SUSPENDED) {
            status=recv_wait_call_prepare(&owner->admission,channel,false,&sent,&sa,&sender);
            if (status==XR_XIR_CALL_READY) {
                XrXirCallResult result=xr_xir_call_poll_bounded(sender,8);status=result.status;
                if (status==XR_XIR_CALL_RETURNED) {
                    CHECK(result.value.payload==1);result=xr_xir_call_poll_bounded(receiver,8);status=result.status;
                    CHECK(status==XR_XIR_CALL_RETURNED && result.value.payload==701);
                }
            }
        }
    }
    CHECK(xr_xir_call_free(sender)==XR_XIR_CALL_READY && xr_xir_call_free(receiver)==XR_XIR_CALL_READY);
    CHECK(!ra.live_bytes && !sa.live_bytes && ra.allocations==ra.frees && sa.allocations==sa.frees);
    XirChannel *storage=(XirChannel *)object_pointer(channel);CHECK(!storage->recv_count && !storage->send_count && !storage->watch_count);return status;
}
static void recv_wait_call_faults(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    size_t live=runtime_memory.live,bytes=runtime_memory.bytes,sites=0;
    for (size_t ordinal=0;ordinal<=sites;++ordinal) {
        size_t begin=runtime_memory.attempts;runtime_memory.fail_at=ordinal ? begin+ordinal-1 : SIZE_MAX;
        XrXirCallStatus status=recv_wait_call_fault_operation(&owner,&channel);size_t attempts=runtime_memory.attempts-begin;runtime_memory.fail_at=SIZE_MAX;
        if (!ordinal) { CHECK(status==XR_XIR_CALL_RETURNED);sites=attempts;CHECK(sites); }
        else CHECK(status==XR_XIR_CALL_OOM && attempts>=ordinal);
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);CHECK(recv_wait_call_fault_operation(&owner,&channel)==XR_XIR_CALL_RETURNED);
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
    }
    printf("Receive-first real two-Call pipeline complete runtime allocator ordinals=%zu; exact native Recv inbox and same-owner recovery physical baseline\n",sites);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void recv_wait_call_cases(void) {
    for (unsigned closed=0;closed<2;++closed) for (unsigned cross=0;cross<2;++cross) recv_wait_call_pair(closed!=0,cross!=0);
    for (unsigned direct=0;direct<2;++direct) for (unsigned last=0;last<2;++last) recv_wait_call_cancel(direct!=0,last!=0);
    for (unsigned mode=1;mode<=2;++mode) {
        ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
        CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
        RendezvousWitness witness={.mode=mode};XrXirCallAccounting accounting={0};XrXirCall *call=NULL;
        CHECK(recv_wait_call_prepare(&owner.admission,&channel,true,&witness,&accounting,&call)==XR_XIR_CALL_READY);
        XrXirCallStatus expected=mode==1 ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_CANCELLED;
        CHECK(xr_xir_call_poll_bounded(call,8).status==expected && witness.releases==1 && witness.reason==expected && !call->channel_recv);
        CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);xr_xir_value_drop(&channel);result_owner_free(&owner);
    }
    recv_wait_call_faults();puts("Receive-first Call: unique owner, actual Recv inbox,256 pending polls, same/cross-Domain Value/Closed, held-lock cancellation/free, malformed action and callback cancellation physical0");
}
#endif // XIR_CHANNEL_RECV_WAIT_CALL_CASES_H
