/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_recv_wait_fault_cases.h - Exact allocation and work frontiers
 */
#ifndef XIR_CHANNEL_RECV_WAIT_FAULT_CASES_H
#define XIR_CHANNEL_RECV_WAIT_FAULT_CASES_H
static void recv_wait_begin_faults(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XirChannel *storage=(XirChannel *)object_pointer(&channel);size_t sites=0;
    for (size_t ordinal=0;ordinal<=sites;++ordinal) {
        XrXirChannelWatch *watch=NULL;XrXirChannelWatchState observed=XR_XIR_CHANNEL_WATCH_RETRY;
        CHECK(xr_xir_channel_watch_begin(&channel,channel_call_sequence(&owner,&channel),&owner.admission,&watch,&observed)==XR_XIR_VALUE_OK && watch);
        uint64_t sequence=storage->sequence;size_t live=runtime_memory.live,bytes=runtime_memory.bytes,begin=runtime_memory.attempts;
        runtime_memory.fail_at=ordinal ? begin+ordinal-1 : SIZE_MAX;
        XrXirChannelRecvWait *recv=NULL;XrXirChannelRecvWaitState state=XR_XIR_CHANNEL_RECV_WAIT_VALUE;
        XrXirValueStatus status=xr_xir_channel_rendezvous_recv_begin(&channel,(XrXirType)RESULT_I64,&owner.admission,&recv,&state);
        size_t attempts=runtime_memory.attempts-begin;runtime_memory.fail_at=SIZE_MAX;
        if (!ordinal) { CHECK(status==XR_XIR_VALUE_OK && recv);sites=attempts;CHECK(sites==2);xr_xir_channel_recv_wait_drop(recv); }
        else CHECK(status==XR_XIR_VALUE_OOM && attempts>=ordinal && !recv && state==XR_XIR_CHANNEL_RECV_WAIT_VALUE &&
            !storage->recv_count && storage->sequence==sequence && !xr_xir_channel_watch_ready(watch));
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes && !storage->recv_count);
        recv=recv_wait_begin(&owner,&channel,(XrXirType)RESULT_I64);xr_xir_channel_recv_wait_drop(recv);
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);xr_xir_channel_watch_drop(watch);
    }
    printf("Receive reservation complete runtime allocator ordinals=%zu; unchanged queue/sequence/watch and same-owner recovery\n",sites);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void recv_wait_send_faults(bool array) {
    size_t sites=0;
    for (size_t ordinal=0;ordinal<=sites;++ordinal) {
        ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message={0},output={0};
        rendezvous_fault_message(&owner,array,&channel,&message);XrXirValue original=message;
        XrXirType type=(XrXirType)(array ? RESULT_ARRAY_I64 : RESULT_I64);
        XrXirChannelRecvWait *recv=recv_wait_begin(&owner,&channel,type);XirChannel *storage=(XirChannel *)object_pointer(&channel);
        XrXirChannelWatch *watch=NULL;XrXirChannelWatchState observed=XR_XIR_CHANNEL_WATCH_RETRY;
        CHECK(xr_xir_channel_watch_begin(&channel,channel_call_sequence(&owner,&channel),&owner.admission,&watch,&observed)==XR_XIR_VALUE_OK && watch);
        size_t live=runtime_memory.live,bytes=runtime_memory.bytes,begin=runtime_memory.attempts;uint64_t sequence=storage->sequence;
        runtime_memory.fail_at=ordinal ? begin+ordinal-1 : SIZE_MAX;XrXirChannelState state=XR_XIR_CHANNEL_EMPTY;
        XrXirValueStatus status=xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state);
        size_t attempts=runtime_memory.attempts-begin;runtime_memory.fail_at=SIZE_MAX;
        if (!ordinal) { CHECK(status==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT);sites=attempts; }
        else {
            CHECK(status==XR_XIR_VALUE_OOM && attempts>=ordinal && unit_value(&output) && state==XR_XIR_CHANNEL_EMPTY);
            CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes && storage->sequence==sequence && storage->recv_count==1 &&
                xr_xir_channel_recv_wait_state(recv)==XR_XIR_CHANNEL_RECV_WAIT_PENDING && !xr_xir_channel_watch_ready(watch));
            CHECK(recv->reservation && !recv->reservation->object.graph_registered && unit_value(&recv->result));
            CHECK(xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT);
        }
        CHECK(!memcmp(&message,&original,sizeof(message)) && xr_xir_channel_watch_ready(watch));result_variant(&output,0,0);xr_xir_value_drop(&output);
        output=recv_wait_take(recv,0);xr_xir_value_drop(&output);xr_xir_channel_watch_drop(watch);
        xr_xir_value_drop(&message);xr_xir_value_drop(&channel);result_owner_free(&owner);
    }
    printf("Receive-first trySend array=%u complete runtime allocator ordinals=%zu; same-node recovery before transfer\n",array,sites);
}
typedef struct RecvWaitWork {
    ResultOwner owner;
    XrXirValue channel,message,output;
    XrXirChannelRecvWait *recv,*other;
    XrXirChannelWatch *watch;
} RecvWaitWork;
static void recv_wait_work_prepare(RecvWaitWork *test,unsigned mode) {
    result_owner_ready(&test->owner);test->message=result_integer(51);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&test->owner.admission,&test->channel)==XR_XIR_VALUE_OK);
    if (mode) test->recv=recv_wait_begin(&test->owner,&test->channel,(XrXirType)RESULT_I64);
    if (mode>=3) test->other=recv_wait_begin(&test->owner,&test->channel,(XrXirType)RESULT_I64);
    if (mode>=4) {
        XirChannel *storage=(XirChannel *)object_pointer(&test->channel);atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
        xr_xir_channel_recv_wait_drop(test->recv);test->recv=NULL;atomic_store_explicit(&storage->locked,false,memory_order_release);
    }
    XrXirChannelWatchState state=XR_XIR_CHANNEL_WATCH_RETRY;
    CHECK(xr_xir_channel_watch_begin(&test->channel,channel_call_sequence(&test->owner,&test->channel),
        &test->owner.admission,&test->watch,&state)==XR_XIR_VALUE_OK && test->watch);
}
static XrXirValueStatus recv_wait_work_operation(RecvWaitWork *test,unsigned mode,XrXirChannelRecvWait **created,
    XrXirChannelRecvWaitState *waiting,XrXirChannelState *state) {
    if (!mode || mode==4) return xr_xir_channel_rendezvous_recv_begin(&test->channel,(XrXirType)RESULT_I64,&test->owner.admission,created,waiting);
    if (mode==3) return xr_xir_channel_close(&test->channel,&test->owner.admission,state);
    if (mode==2) return xr_xir_channel_send_owned(&test->channel,&test->message,&test->owner.admission,state);
    return xr_xir_channel_try_send(&test->channel,&test->message,(XrXirType)RESULT_SEND,&test->owner.admission,&test->output,state);
}
static void recv_wait_work_drop(RecvWaitWork *test,XrXirChannelRecvWait *created) {
    xr_xir_channel_watch_drop(test->watch);xr_xir_channel_recv_wait_drop(created);
    xr_xir_channel_recv_wait_drop(test->recv);xr_xir_channel_recv_wait_drop(test->other);
    xr_xir_value_drop(&test->output);xr_xir_value_drop(&test->message);xr_xir_value_drop(&test->channel);result_owner_free(&test->owner);
}
static void recv_wait_work_prefixes(void) {
    uint64_t total=0;
    for (unsigned mode=0;mode<6;++mode) {
        RecvWaitWork test={0};recv_wait_work_prepare(&test,mode);uint64_t before=test.owner.admission.work;
        XrXirChannelRecvWait *created=NULL;XrXirChannelRecvWaitState waiting=XR_XIR_CHANNEL_RECV_WAIT_CLOSED;
        XrXirChannelState state=XR_XIR_CHANNEL_EMPTY;
        CHECK(recv_wait_work_operation(&test,mode,&created,&waiting,&state)==XR_XIR_VALUE_OK);
        uint64_t required=before-test.owner.admission.work;CHECK(required && required<256);recv_wait_work_drop(&test,created);
        for (uint64_t cut=0;cut<=required;++cut) {
            test=(RecvWaitWork){0};recv_wait_work_prepare(&test,mode);XirChannel *storage=(XirChannel *)object_pointer(&test.channel);
            uint64_t sequence=storage->sequence;uint32_t count=storage->recv_count;size_t live=runtime_memory.live,bytes=runtime_memory.bytes;
            created=NULL;waiting=XR_XIR_CHANNEL_RECV_WAIT_CLOSED;state=XR_XIR_CHANNEL_EMPTY;test.owner.admission.work=cut;
            XrXirValueStatus status=recv_wait_work_operation(&test,mode,&created,&waiting,&state);
            CHECK(status==(cut==required ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT));
            if (cut<required) {
                CHECK(!created && waiting==XR_XIR_CHANNEL_RECV_WAIT_CLOSED && state==XR_XIR_CHANNEL_EMPTY && unit_value(&test.output) && test.message.payload==51);
                CHECK(storage->sequence==sequence && storage->recv_count==count && !atomic_load_explicit(&storage->closed,memory_order_acquire));
                CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes && !xr_xir_channel_watch_ready(test.watch));
                if (test.recv) CHECK(xr_xir_channel_recv_wait_state(test.recv)==XR_XIR_CHANNEL_RECV_WAIT_PENDING);
                if (test.other) CHECK(xr_xir_channel_recv_wait_state(test.other)==XR_XIR_CHANNEL_RECV_WAIT_PENDING);
            } else CHECK(xr_xir_channel_watch_ready(test.watch));
            test.owner.admission.work=128000000;recv_wait_work_drop(&test,created);
        }
        printf("Receive wait work mode=%u exact=%" PRIu64 " all_prefixes=%" PRIu64 "\n",mode,required,required+1);total+=required+1;
    }
    printf("Receive wait complete local work prefixes=%" PRIu64 "; queue, raw reservation, result, message and observers unchanged on LIMIT\n",total);
}
static void recv_wait_retired_and_limits(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    _Alignas(8) unsigned char prefix[8]={0x58,0x52,0x44,0x4d,0x1b,0,0,1};
    XrXirValueAdmission admission=owner.admission;admission.domain=(XrXirDomain *)(void *)prefix;
    size_t begin=runtime_memory.attempts;XrXirChannelRecvWait *recv=NULL;XrXirChannelRecvWaitState state=XR_XIR_CHANNEL_RECV_WAIT_VALUE;
    CHECK(xr_xir_channel_rendezvous_recv_begin(&channel,(XrXirType)RESULT_I64,&admission,&recv,&state)==XR_XIR_VALUE_BAD_ARGUMENT &&
        !recv && state==XR_XIR_CHANNEL_RECV_WAIT_VALUE && runtime_memory.attempts==begin);
    CHECK(xr_xir_channel_rendezvous_recv_begin(&channel,(XrXirType)RESULT_STRING,&owner.admission,&recv,&state)==XR_XIR_VALUE_BAD_ARGUMENT &&
        !recv && state==XR_XIR_CHANNEL_RECV_WAIT_VALUE && runtime_memory.attempts==begin);
    XirObject *object=object_pointer(&channel);uint32_t refs=atomic_load_explicit(&object->references,memory_order_relaxed);
    atomic_store_explicit(&object->references,UINT32_MAX,memory_order_relaxed);
    CHECK(xr_xir_channel_rendezvous_recv_begin(&channel,(XrXirType)RESULT_I64,&owner.admission,&recv,&state)==XR_XIR_VALUE_REFCOUNT_LIMIT && !recv);
    atomic_store_explicit(&object->references,refs,memory_order_relaxed);
    refs=atomic_load_explicit(&owner.domain->references,memory_order_relaxed);atomic_store_explicit(&owner.domain->references,UINT32_MAX,memory_order_relaxed);
    CHECK(xr_xir_channel_rendezvous_recv_begin(&channel,(XrXirType)RESULT_I64,&owner.admission,&recv,&state)==XR_XIR_VALUE_REFCOUNT_LIMIT && !recv);
    atomic_store_explicit(&owner.domain->references,refs,memory_order_relaxed);
    CHECK(runtime_memory.attempts==begin);recv=recv_wait_begin(&owner,&channel,(XrXirType)RESULT_I64);xr_xir_channel_recv_wait_drop(recv);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
    puts("Receive wait retired27 aligned8B Domain and wrong native Recv reject before allocation; Channel/Domain refcount-limit recovery physical0");
}
static void recv_wait_fault_cases(void) {
    recv_wait_begin_faults();recv_wait_send_faults(false);recv_wait_send_faults(true);recv_wait_work_prefixes();recv_wait_retired_and_limits();
}
#endif // XIR_CHANNEL_RECV_WAIT_FAULT_CASES_H
