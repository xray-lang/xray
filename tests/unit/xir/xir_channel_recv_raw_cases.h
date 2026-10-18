/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_recv_raw_cases.h - Bare payload ownership without a Recv allocation
 */
#ifndef XIR_CHANNEL_RECV_RAW_CASES_H
#define XIR_CHANNEL_RECV_RAW_CASES_H
static XrXirChannelRecvWait *recv_raw_begin(ResultOwner *owner,const XrXirValue *channel) {
    XrXirChannelRecvWait *recv=NULL;XrXirChannelRecvWaitState state=XR_XIR_CHANNEL_RECV_WAIT_RETRY;size_t begin=runtime_memory.attempts;
    CHECK(xr_xir_channel_rendezvous_recv_raw_begin(channel,&owner->admission,&recv,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_RECV_WAIT_PENDING && recv && recv->raw && !recv->reservation && unit_value(&recv->closed));
    CHECK(runtime_memory.attempts==begin+1);return recv;
}
static void recv_raw_value(unsigned kind,bool closed) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message={0},output={0};
    XrXirType channel_type=(XrXirType)(kind==0 ? RESULT_CHANNEL_I64 : kind==1 ? RESULT_CHANNEL_UNIT : RESULT_CHANNEL_ARRAY);
    if (!kind) message=result_integer(411);
    if (kind==2) { XrXirValue value=result_integer(11);CHECK(xr_xir_array_new((XrXirType)RESULT_ARRAY,&value,1,&owner.admission,&message)==XR_XIR_VALUE_OK); }
    CHECK(xr_xir_channel_new(channel_type,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);XrXirChannelRecvWait *recv=recv_raw_begin(&owner,&channel);
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;size_t attempts=runtime_memory.attempts;
    if (closed) CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK && xr_xir_channel_recv_wait_state(recv)==XR_XIR_CHANNEL_RECV_WAIT_CLOSED);
    else CHECK(xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_SENT && xr_xir_channel_recv_wait_state(recv)==XR_XIR_CHANNEL_RECV_WAIT_VALUE);
    CHECK(runtime_memory.attempts==attempts+(!closed && kind==2 ? 1u : 0u));xr_xir_value_drop(&output);
    if (!closed && kind==2) {
        XrXirValue replacement=result_integer(99);XrXirValuePlace place={(XrXirType)RESULT_ARRAY,&message.payload};XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_set(&place,0,&replacement,&owner.admission,&fault)==XR_XIR_VALUE_OK);
    }
    xr_xir_value_drop(&message);xr_xir_value_drop(&channel);attempts=runtime_memory.attempts;
    CHECK(xr_xir_channel_recv_wait_take(recv,&output) && recv->taken);
    XrXirValue again={0};CHECK(!xr_xir_channel_recv_wait_take(recv,&again) && unit_value(&again));
    if (closed || kind==1) CHECK(unit_value(&output));
    else if (!kind) CHECK(output.type==XR_XIR_I64 && output.payload==411);
    else { CHECK(output.type==RESULT_ARRAY);const XirArray *array=(const XirArray *)object_pointer(&output);int64_t value=0;memcpy(&value,array->data,8);CHECK(value==11); }
    xr_xir_channel_recv_wait_drop(recv);xr_xir_value_drop(&output);CHECK(runtime_memory.attempts==attempts);result_owner_free(&owner);
}
static void recv_raw_faults(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    size_t live=runtime_memory.live,bytes=runtime_memory.bytes,begin=runtime_memory.attempts;XirChannel *storage=(XirChannel *)object_pointer(&channel);
    uint64_t sequence=storage->sequence;runtime_memory.fail_at=begin;XrXirChannelRecvWait *recv=NULL;XrXirChannelRecvWaitState state=XR_XIR_CHANNEL_RECV_WAIT_VALUE;
    CHECK(xr_xir_channel_rendezvous_recv_raw_begin(&channel,&owner.admission,&recv,&state)==XR_XIR_VALUE_OOM && !recv &&
        state==XR_XIR_CHANNEL_RECV_WAIT_VALUE && !storage->recv_count && storage->sequence==sequence);
    runtime_memory.fail_at=SIZE_MAX;CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes && runtime_memory.attempts==begin+1);
    recv=recv_raw_begin(&owner,&channel);xr_xir_channel_recv_wait_drop(recv);CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
    _Alignas(8) unsigned char prefix[8]={0x58,0x52,0x44,0x4d,0x1c,0,0,1};
    XrXirValueAdmission admission=owner.admission;admission.domain=(XrXirDomain *)(void *)prefix;recv=NULL;begin=runtime_memory.attempts;
    CHECK(xr_xir_channel_rendezvous_recv_raw_begin(&channel,&admission,&recv,&state)==XR_XIR_VALUE_BAD_ARGUMENT && !recv && runtime_memory.attempts==begin);
    xr_xir_value_drop(&channel);result_owner_free(&owner);puts("Bare receive complete allocator ordinal1, same-owner recovery, retired28 aligned8B Domain rejection and physical0");
}
static void recv_raw_work_prepare(RecvWaitWork *test,unsigned mode) {
    result_owner_ready(&test->owner);test->message=result_integer(53);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&test->owner.admission,&test->channel)==XR_XIR_VALUE_OK);
    if (mode) test->recv=recv_raw_begin(&test->owner,&test->channel);
    XrXirChannelWatchState observed=XR_XIR_CHANNEL_WATCH_RETRY;
    CHECK(xr_xir_channel_watch_begin(&test->channel,channel_call_sequence(&test->owner,&test->channel),
        &test->owner.admission,&test->watch,&observed)==XR_XIR_VALUE_OK && test->watch);
}
static XrXirValueStatus recv_raw_work_operation(RecvWaitWork *test,unsigned mode,XrXirChannelRecvWait **created,
    XrXirChannelRecvWaitState *waiting,XrXirChannelState *state) {
    if (!mode) return xr_xir_channel_rendezvous_recv_raw_begin(&test->channel,&test->owner.admission,created,waiting);
    if (mode==1) return xr_xir_channel_send_owned(&test->channel,&test->message,&test->owner.admission,state);
    return xr_xir_channel_close(&test->channel,&test->owner.admission,state);
}
static void recv_raw_work(void) {
    uint64_t total=0;
    for (unsigned mode=0;mode<3;++mode) {
        RecvWaitWork test={0};recv_raw_work_prepare(&test,mode);uint64_t before=test.owner.admission.work;
        XrXirChannelRecvWait *created=NULL;XrXirChannelRecvWaitState waiting=XR_XIR_CHANNEL_RECV_WAIT_VALUE;XrXirChannelState state=XR_XIR_CHANNEL_EMPTY;
        CHECK(recv_raw_work_operation(&test,mode,&created,&waiting,&state)==XR_XIR_VALUE_OK);uint64_t required=before-test.owner.admission.work;
        CHECK(required && required<128);recv_wait_work_drop(&test,created);
        for (uint64_t cut=0;cut<=required;++cut) {
            test=(RecvWaitWork){0};recv_raw_work_prepare(&test,mode);XirChannel *storage=(XirChannel *)object_pointer(&test.channel);
            uint64_t sequence=storage->sequence;uint32_t count=storage->recv_count;size_t live=runtime_memory.live,bytes=runtime_memory.bytes;
            created=NULL;waiting=XR_XIR_CHANNEL_RECV_WAIT_VALUE;state=XR_XIR_CHANNEL_EMPTY;test.owner.admission.work=cut;
            XrXirValueStatus status=recv_raw_work_operation(&test,mode,&created,&waiting,&state);CHECK(status==(cut==required ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT));
            if (cut<required) {
                CHECK(!created && waiting==XR_XIR_CHANNEL_RECV_WAIT_VALUE && state==XR_XIR_CHANNEL_EMPTY && test.message.payload==53);
                CHECK(storage->sequence==sequence && storage->recv_count==count && !atomic_load_explicit(&storage->closed,memory_order_acquire));
                CHECK(!xr_xir_channel_watch_ready(test.watch) && runtime_memory.live==live && runtime_memory.bytes==bytes);
                if (test.recv) CHECK(!test.recv->taken && unit_value(&test.recv->result) && xr_xir_channel_recv_wait_state(test.recv)==XR_XIR_CHANNEL_RECV_WAIT_PENDING);
            } else CHECK(xr_xir_channel_watch_ready(test.watch));
            test.owner.admission.work=128000000;recv_wait_work_drop(&test,created);
        }
        total+=required+1;printf("Bare receive work mode=%u exact=%" PRIu64 " all_prefixes=%" PRIu64 "\n",mode,required,required+1);
    }
    printf("Bare receive complete local work prefixes=%" PRIu64 "; no Recv wrapper publication or allocation\n",total);
}
static XrXirAction recv_raw_call_entry(XrXirCallView *view) {
    RecvWaitFrame *frame=view->state;RendezvousWitness *w=(RendezvousWitness *)view->environment;++w->callbacks;
    if (!frame->started) {
        XrXirChannelRecvWaitState state=XR_XIR_CHANNEL_RECV_WAIT_RETRY;
        XrXirValueStatus status=xr_xir_channel_rendezvous_recv_raw_begin(&view->arguments[0],xr_xir_call_admission(view),&frame->recv,&state);
        if (status!=XR_XIR_VALUE_OK) return recv_wait_fault(status);
        CHECK(state==XR_XIR_CHANNEL_RECV_WAIT_PENDING);frame->started=true;
        size_t attempts=runtime_memory.attempts;CHECK(xr_xir_call_wait_channel_recv(view,&frame->recv)==XR_XIR_CALL_SUSPENDED && !frame->recv && runtime_memory.attempts==attempts);
        return channel_call_action(XR_XIR_ACTION_AWAIT_CHANNEL,(XrXirValue){0});
    }
    CHECK(view->inbox.status==XR_XIR_CALL_RETURNED && (view->inbox.value.type==XR_XIR_UNIT || view->inbox.value.type==XR_XIR_I64));
    return channel_call_action(XR_XIR_ACTION_RETURN,result_integer(view->inbox.value.type==XR_XIR_UNIT ? 333 : view->inbox.value.payload));
}
static void recv_raw_call(bool closed) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(419);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    const XrXirType parameter=(XrXirType)RESULT_CHANNEL_I64;RendezvousWitness witness={0};XrXirCallAccounting accounting={0};
    XrXirCallEntry entry={XR_XIR_CALL_ABI_VERSION,&parameter,1,XR_XIR_I64,sizeof(RecvWaitFrame),recv_raw_call_entry,recv_wait_call_release,&witness,0,0};
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=&accounting;config.admission=owner.admission;XrXirCall *call=NULL;
    CHECK(xr_xir_call_new(&config,0,&channel,1,&call)==XR_XIR_CALL_READY && xr_xir_call_poll_bounded(call,1).status==XR_XIR_CALL_SUSPENDED);
    size_t attempts=runtime_memory.attempts;XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    if (closed) CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK);
    else CHECK(xr_xir_channel_send_owned(&channel,&message,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT);
    XrXirCallResult result=xr_xir_call_poll_bounded(call,8);CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.payload==(closed ? 333 : 419));
    CHECK(runtime_memory.attempts==attempts && witness.releases==1 && xr_xir_call_free(call)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&message);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void recv_raw_cases(void) {
    for (unsigned kind=0;kind<3;++kind) { recv_raw_value(kind,false);recv_raw_value(kind,true); }
    recv_raw_faults();recv_raw_work();recv_raw_call(false);recv_raw_call(true);
    puts("Bare receive owned i64/Unit/Array or Closed: one node, no native Recv allocation, take-once Unit, logical COW and real Call raw inbox physical0");
}
#endif // XIR_CHANNEL_RECV_RAW_CASES_H
