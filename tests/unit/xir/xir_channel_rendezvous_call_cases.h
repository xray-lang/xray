/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_rendezvous_call_cases.h - Native frame ownership across real rendezvous
 */
#ifndef XIR_CHANNEL_RENDEZVOUS_CALL_CASES_H
#define XIR_CHANNEL_RENDEZVOUS_CALL_CASES_H
typedef struct RendezvousFrame { XrXirChannelSendWait *send;XrXirValue received;uint32_t phase; } RendezvousFrame;
typedef struct RendezvousWitness { unsigned callbacks,releases,mode;XrXirCallStatus reason; } RendezvousWitness;
static XrXirAction rendezvous_sender_entry(XrXirCallView *view) {
    RendezvousFrame *frame=view->state;RendezvousWitness *w=(RendezvousWitness *)view->environment;++w->callbacks;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);CHECK(admission && view->argument_count==1);
    if (!frame->phase) {
        XrXirValue message=result_integer(541);XrXirChannelSendWaitState state=XR_XIR_CHANNEL_SEND_WAIT_RETRY;
        XrXirValueStatus status=xr_xir_channel_rendezvous_send_begin(&view->arguments[0],&message,admission,&frame->send,&state);
        if (status==XR_XIR_VALUE_OOM || status==XR_XIR_VALUE_LIMIT)
            return xr_xir_call_fault(status==XR_XIR_VALUE_OOM ? XR_XIR_RUN_OUT_OF_MEMORY : XR_XIR_RUN_FRAME_LIMIT);
        CHECK(status==XR_XIR_VALUE_OK && frame->send);
        frame->phase=1;
    }
    if (!frame->send) {
        CHECK(view->inbox.status==XR_XIR_CALL_RETURNED && view->inbox.value.type==XR_XIR_I64);
        return channel_call_action(XR_XIR_ACTION_RETURN,view->inbox.value);
    }
    XrXirCallView copy=*view;size_t attempts=runtime_memory.attempts;
    CHECK(xr_xir_call_wait_channel_send(&copy,&frame->send)==XR_XIR_CALL_BAD_STATE);
    XrXirCallStatus waiting=xr_xir_call_wait_channel_send(view,&frame->send);
    CHECK(runtime_memory.attempts==attempts && (waiting==XR_XIR_CALL_READY || waiting==XR_XIR_CALL_SUSPENDED));
    if (waiting==XR_XIR_CALL_READY) return channel_call_action(XR_XIR_ACTION_RETURN,result_integer(xr_xir_channel_send_wait_state(frame->send)));
    CHECK(!frame->send);
    if (w->mode==1) return channel_call_action(XR_XIR_ACTION_CONTINUE,(XrXirValue){0});
    if (w->mode==2) CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_CANCEL_REQUESTED);
    return channel_call_action(XR_XIR_ACTION_AWAIT_CHANNEL,(XrXirValue){0});
}
static XrXirAction rendezvous_receiver_entry(XrXirCallView *view) {
    RendezvousFrame *frame=view->state;RendezvousWitness *w=(RendezvousWitness *)view->environment;++w->callbacks;
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    XrXirValueStatus status=xr_xir_channel_receive_result(&view->arguments[0],(XrXirType)RESULT_I64,xr_xir_call_admission(view),&frame->received,&state);
    if (status==XR_XIR_VALUE_OOM || status==XR_XIR_VALUE_LIMIT)
        return xr_xir_call_fault(status==XR_XIR_VALUE_OOM ? XR_XIR_RUN_OUT_OF_MEMORY : XR_XIR_RUN_FRAME_LIMIT);
    CHECK(status==XR_XIR_VALUE_OK && (state==XR_XIR_CHANNEL_VALUE || state==XR_XIR_CHANNEL_EMPTY));
    int64_t number=-1;
    if (state==XR_XIR_CHANNEL_VALUE) {
        XrXirEnumBorrow borrow={0};CHECK(xr_xir_enum_borrow(&frame->received,&borrow)==XR_XIR_VALUE_OK && borrow.field_count==1);
        number=borrow.fields[0].payload;
    }
    return channel_call_action(XR_XIR_ACTION_RETURN,result_integer(number));
}
static void rendezvous_call_release(XrXirCallView *view,XrXirCallStatus reason) {
    RendezvousFrame *frame=view->state;RendezvousWitness *w=(RendezvousWitness *)view->environment;
    XrXirChannelSendWait *send=frame->send;frame->send=NULL;
    xr_xir_channel_send_wait_drop(send);xr_xir_value_drop(&frame->received);++w->releases;w->reason=reason;
}
static XrXirCallStatus rendezvous_call_prepare(const XrXirValueAdmission *admission,const XrXirValue *channel,
    bool receiver,RendezvousWitness *w,XrXirCallAccounting *accounting,XrXirCall **output) {
    const XrXirType parameter=(XrXirType)RESULT_CHANNEL_I64;
    XrXirCallEntry entry={XR_XIR_CALL_ABI_VERSION,&parameter,1,XR_XIR_I64,sizeof(RendezvousFrame),
        receiver ? rendezvous_receiver_entry : rendezvous_sender_entry,rendezvous_call_release,w,0,0};
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=accounting;config.admission=*admission;
    return xr_xir_call_new(&config,0,channel,1,output);
}
static XrXirCall *rendezvous_call_new(const XrXirValueAdmission *admission,const XrXirValue *channel,
    bool receiver,RendezvousWitness *w,XrXirCallAccounting *accounting) {
    XrXirCall *call=NULL;CHECK(rendezvous_call_prepare(admission,channel,receiver,w,accounting,&call)==XR_XIR_CALL_READY);return call;
}
static XrXirCall *rendezvous_claim_sender;
static void rendezvous_claim_before_ready(void) { CHECK(!xr_xir_call_channel_ready(rendezvous_claim_sender)); }
static void rendezvous_call_pair(bool cross_domain) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirValueAdmission receiver_admission=owner.admission;XrXirDomain *peer=NULL;
    if (cross_domain) {
        XrXirDomainBudgetControls controls={8388608,8388608,128000000,4194304,4194304};
        CHECK(xr_xir_domain_new_budgeted(8388608,&controls,&peer)==XR_XIR_VALUE_OK);receiver_admission.domain=peer;
    }
    RendezvousWitness sent={0},received={0};XrXirCallAccounting sender_accounting={0},receiver_accounting={0};
    XrXirCall *sender=rendezvous_call_new(&owner.admission,&channel,false,&sent,&sender_accounting);
    CHECK(xr_xir_call_poll_bounded(sender,1).status==XR_XIR_CALL_SUSPENDED);
    size_t attempts=runtime_memory.attempts;
    for (unsigned i=0;i<256;++i) CHECK(xr_xir_call_poll_bounded(sender,1).status==XR_XIR_CALL_SUSPENDED);
    CHECK(sent.callbacks==1 && runtime_memory.attempts==attempts);
    CHECK(sender->channel_send && !sender->channel_watch && !((RendezvousFrame *)sender->top->state)->send);
    rendezvous_claim_sender=sender;
    channel_cas32_target=&sender->channel_send->state;channel_cas32_count=0;channel_cas32_pause_at=1;channel_cas32_before=rendezvous_claim_before_ready;
    XrXirCall *receiver=rendezvous_call_new(&receiver_admission,&channel,true,&received,&receiver_accounting);
    XrXirCallResult result=xr_xir_call_poll_bounded(receiver,4);
    CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.payload==541 && !channel_cas32_before);
    channel_cas32_target=NULL;rendezvous_claim_sender=NULL;CHECK(xr_xir_call_channel_ready(sender));
    CHECK(xr_xir_call_poll_bounded(sender,1).status==XR_XIR_CALL_READY && sent.callbacks==1);
    result=xr_xir_call_poll_bounded(sender,4);
    CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.payload==XR_XIR_CHANNEL_SEND_WAIT_SENT);
    CHECK(sent.releases==1 && received.releases==1 && !((XirChannel *)object_pointer(&channel))->send_count);
    CHECK(xr_xir_call_free(sender)==XR_XIR_CALL_READY && xr_xir_call_free(receiver)==XR_XIR_CALL_READY);
    CHECK(!sender_accounting.live_bytes && !receiver_accounting.live_bytes);
    xr_xir_domain_drop(peer);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void rendezvous_call_cancel(bool direct_free,bool last) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    RendezvousWitness sent={0};XrXirCallAccounting accounting={0};
    XrXirCall *sender=rendezvous_call_new(&owner.admission,&channel,false,&sent,&accounting);
    CHECK(xr_xir_call_poll_bounded(sender,1).status==XR_XIR_CALL_SUSPENDED);
    XirChannel *storage=(XirChannel *)object_pointer(&channel);atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    if (last) xr_xir_value_drop(&channel);
    if (!direct_free) {
        CHECK(xr_xir_call_request_cancel(sender)==XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_call_poll_bounded(sender,8).status==XR_XIR_CALL_CANCELLED);
    }
    CHECK(xr_xir_call_free(sender)==XR_XIR_CALL_READY && sent.releases==1 && sent.reason==XR_XIR_CALL_CANCELLED);
    if (!last) {
        atomic_store_explicit(&storage->locked,false,memory_order_release);
        CHECK(storage->send_count==1 && atomic_load_explicit(&storage->send_head->state,memory_order_acquire)==CHANNEL_SEND_CANCELLED);
        RendezvousWitness received={0};XrXirCallAccounting receiver_accounting={0};
        XrXirCall *receiver=rendezvous_call_new(&owner.admission,&channel,true,&received,&receiver_accounting);
        XrXirCallResult result=xr_xir_call_poll_bounded(receiver,4);CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.payload==-1);
        CHECK(xr_xir_call_free(receiver)==XR_XIR_CALL_READY);
        XrXirChannelState state=XR_XIR_CHANNEL_RETRY;CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK && !storage->send_count);
        xr_xir_value_drop(&channel);
    }
    result_owner_free(&owner);
}
static void rendezvous_call_cases(void) {
    rendezvous_call_pair(false);rendezvous_call_pair(true);
    for (unsigned direct=0;direct<2;++direct) for (unsigned last=0;last<2;++last) rendezvous_call_cancel(direct!=0,last!=0);
    puts("Rendezvous real native Call frames: same/cross-Domain receive541, direct owned wait without post-publication allocation,256 pending polls and four held-lock cancellation/free cases physical0");
}
static XrXirCallStatus rendezvous_call_fault_operation(ResultOwner *owner,const XrXirValue *channel) {
    RendezvousWitness sent={0},received={0};XrXirCallAccounting sa={0},ra={0};XrXirCall *sender=NULL,*receiver=NULL;
    XrXirCallStatus status=rendezvous_call_prepare(&owner->admission,channel,false,&sent,&sa,&sender);
    if (status==XR_XIR_CALL_READY) {
        status=xr_xir_call_poll_bounded(sender,1).status;
        if (status==XR_XIR_CALL_READY) status=xr_xir_call_poll_bounded(sender,8).status;
        if (status==XR_XIR_CALL_SUSPENDED) {
            status=rendezvous_call_prepare(&owner->admission,channel,true,&received,&ra,&receiver);
            if (status==XR_XIR_CALL_READY) {
                XrXirCallResult result=xr_xir_call_poll_bounded(receiver,8);status=result.status;
                if (status==XR_XIR_CALL_RETURNED) {
                    CHECK(result.value.payload==541);result=xr_xir_call_poll_bounded(sender,8);status=result.status;
                    CHECK(status==XR_XIR_CALL_RETURNED && result.value.payload==XR_XIR_CHANNEL_SEND_WAIT_SENT);
                } else {
                    CHECK(status==XR_XIR_CALL_OOM && xr_xir_channel_send_wait_state(sender->channel_send)==XR_XIR_CHANNEL_SEND_WAIT_PENDING);
                }
            }
        }
    }
    CHECK(xr_xir_call_free(receiver)==XR_XIR_CALL_READY && xr_xir_call_free(sender)==XR_XIR_CALL_READY);
    CHECK(!sa.live_bytes && !ra.live_bytes && sa.allocations==sa.frees && ra.allocations==ra.frees);
    XirChannel *storage=(XirChannel *)object_pointer(channel);CHECK(!storage->send_count && !storage->watch_count);
    return status;
}
static void rendezvous_call_faults(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    size_t live=runtime_memory.live,bytes=runtime_memory.bytes,sites=0;
    for (size_t ordinal=0;ordinal<=sites;++ordinal) {
        size_t begin=runtime_memory.attempts;runtime_memory.fail_at=ordinal ? begin+ordinal-1 : SIZE_MAX;
        XrXirCallStatus status=rendezvous_call_fault_operation(&owner,&channel);
        size_t attempts=runtime_memory.attempts-begin;runtime_memory.fail_at=SIZE_MAX;
        if (!ordinal) { CHECK(status==XR_XIR_CALL_RETURNED);sites=attempts;CHECK(sites); }
        else CHECK(status==XR_XIR_CALL_OOM && attempts>=ordinal);
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
        CHECK(rendezvous_call_fault_operation(&owner,&channel)==XR_XIR_CALL_RETURNED);
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
    }
    printf("Rendezvous real two-Call pipeline complete runtime allocator ordinals=%zu; before-ack receive OOM and same-owner recovery physical baseline\n",sites);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static XrXirAction rendezvous_owner_entry(XrXirCallView *view) {
    XrXirChannelSendWait **owner=(XrXirChannelSendWait **)view->environment;XrXirChannelSendWait *before=*owner;
    bool same=before->domain==xr_xir_call_admission(view)->domain;size_t attempts=runtime_memory.attempts;
    XrXirCallStatus status=xr_xir_call_wait_channel_send(view,owner);
    CHECK(status==(same ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_ARGUMENT) && *owner==before && runtime_memory.attempts==attempts);
    return channel_call_action(XR_XIR_ACTION_RETURN,result_integer(status));
}
static void rendezvous_call_owner(bool foreign) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(89);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelSendWait *send=rendezvous_begin(&owner,&channel,&message);XrXirDomain *peer=NULL;
    if (!foreign) CHECK(rendezvous_receive(&owner,&channel,true)==89);
    else CHECK(xr_xir_domain_new(8388608,&peer)==XR_XIR_VALUE_OK);
    XrXirCallEntry entry={XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_I64,0,rendezvous_owner_entry,NULL,&send,0,0};
    XrXirCallAccounting accounting={0};XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=&accounting;config.admission=owner.admission;
    if (peer) config.admission.domain=peer;
    XrXirCall *call=NULL;CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_READY);
    XrXirCallResult result=xr_xir_call_poll_bounded(call,4);CHECK(result.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);xr_xir_domain_drop(peer);
    if (foreign) CHECK(rendezvous_receive(&owner,&channel,true)==89);
    xr_xir_channel_send_wait_drop(send);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void rendezvous_call_staged_rejection(void) {
    for (unsigned mode=1;mode<=2;++mode) {
        ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
        CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
        RendezvousWitness witness={.mode=mode};XrXirCallAccounting accounting={0};
        XrXirCall *call=rendezvous_call_new(&owner.admission,&channel,false,&witness,&accounting);
        XrXirCallStatus expected=mode==1 ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_CANCELLED;
        CHECK(xr_xir_call_poll_bounded(call,8).status==expected && witness.releases==1 && witness.reason==expected);
        CHECK(!((XirChannel *)object_pointer(&channel))->send_count && !call->channel_send);
        CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);xr_xir_value_drop(&channel);result_owner_free(&owner);
    }
}
#endif // XIR_CHANNEL_RENDEZVOUS_CALL_CASES_H
