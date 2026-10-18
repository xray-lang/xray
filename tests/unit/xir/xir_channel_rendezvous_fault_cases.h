/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_rendezvous_fault_cases.h - Full physical allocator and work frontiers
 */
#ifndef XIR_CHANNEL_RENDEZVOUS_FAULT_CASES_H
#define XIR_CHANNEL_RENDEZVOUS_FAULT_CASES_H
static void rendezvous_fault_message(ResultOwner *owner,bool array,XrXirValue *channel,XrXirValue *message) {
    if (array) {
        XrXirValue number=result_integer(17);
        CHECK(xr_xir_array_new((XrXirType)RESULT_ARRAY,&number,1,&owner->admission,message)==XR_XIR_VALUE_OK);
    } else *message=result_integer(17);
    CHECK(xr_xir_channel_new((XrXirType)(array ? RESULT_CHANNEL_ARRAY : RESULT_CHANNEL_I64),0,&owner->admission,channel)==XR_XIR_VALUE_OK);
}
static void rendezvous_begin_faults(bool array) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message={0};
    rendezvous_fault_message(&owner,array,&channel,&message);XirChannel *storage=(XirChannel *)object_pointer(&channel);
    size_t live=runtime_memory.live,bytes=runtime_memory.bytes,sites=0;
    for (size_t ordinal=0;ordinal<=sites;++ordinal) {
        size_t begin=runtime_memory.attempts;uint64_t sequence=storage->sequence;
        runtime_memory.fail_at=ordinal ? begin+ordinal-1 : SIZE_MAX;
        XrXirChannelSendWait *send=NULL;XrXirChannelSendWaitState state=XR_XIR_CHANNEL_SEND_WAIT_CLOSED;
        XrXirValueStatus status=xr_xir_channel_rendezvous_send_begin(&channel,&message,&owner.admission,&send,&state);
        size_t attempts=runtime_memory.attempts-begin;runtime_memory.fail_at=SIZE_MAX;
        if (!ordinal) { CHECK(status==XR_XIR_VALUE_OK && send);sites=attempts;CHECK(sites);xr_xir_channel_send_wait_drop(send); }
        else CHECK(status==XR_XIR_VALUE_OOM && attempts>=ordinal && !send &&
            state==XR_XIR_CHANNEL_SEND_WAIT_CLOSED && storage->sequence==sequence);
        CHECK(!storage->send_count && runtime_memory.live==live && runtime_memory.bytes==bytes);
        send=rendezvous_begin(&owner,&channel,&message);xr_xir_channel_send_wait_drop(send);
        CHECK(!storage->send_count && runtime_memory.live==live && runtime_memory.bytes==bytes);
    }
    printf("Rendezvous begin array=%u complete runtime allocator ordinals=%zu; same-owner recovery and physical baseline\n",array,sites);
    xr_xir_value_drop(&message);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void rendezvous_receive_faults(bool array) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message={0};
    rendezvous_fault_message(&owner,array,&channel,&message);XirChannel *storage=(XirChannel *)object_pointer(&channel);
    XrXirType result_type=(XrXirType)(array ? RESULT_ARRAY_I64 : RESULT_I64);size_t sites=0;
    for (size_t ordinal=0;ordinal<=sites;++ordinal) {
        XrXirChannelSendWait *send=rendezvous_begin(&owner,&channel,&message);XrXirValue output={0};
        size_t live=runtime_memory.live,bytes=runtime_memory.bytes,begin=runtime_memory.attempts;
        uint64_t sequence=storage->sequence;runtime_memory.fail_at=ordinal ? begin+ordinal-1 : SIZE_MAX;
        XrXirChannelState state=XR_XIR_CHANNEL_FULL;
        XrXirValueStatus status=xr_xir_channel_receive_result(&channel,result_type,&owner.admission,&output,&state);
        size_t attempts=runtime_memory.attempts-begin;runtime_memory.fail_at=SIZE_MAX;
        if (!ordinal) { CHECK(status==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);sites=attempts;CHECK(sites); }
        else {
            CHECK(status==XR_XIR_VALUE_OOM && attempts>=ordinal && unit_value(&output) && state==XR_XIR_CHANNEL_FULL);
            CHECK(storage->send_count==1 && storage->sequence==sequence && xr_xir_channel_send_wait_state(send)==XR_XIR_CHANNEL_SEND_WAIT_PENDING);
            CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes &&
                atomic_load_explicit(&send->references,memory_order_relaxed)==2);
            CHECK(xr_xir_channel_receive_result(&channel,result_type,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
        }
        result_variant(&output,0,1);CHECK(xr_xir_channel_send_wait_state(send)==XR_XIR_CHANNEL_SEND_WAIT_SENT);
        xr_xir_value_drop(&output);xr_xir_channel_send_wait_drop(send);CHECK(!storage->send_count);
    }
    printf("Rendezvous Recv array=%u complete runtime allocator ordinals=%zu; payload/ack preserved before allocation and same-node recovery\n",array,sites);
    xr_xir_value_drop(&message);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
typedef struct RendezvousWork {
    ResultOwner owner;
    XrXirValue channel,message,output;
    XrXirChannelSendWait *send,*other;
    XrXirChannelWatch *watch;
} RendezvousWork;
static void rendezvous_work_prepare(RendezvousWork *test,unsigned mode) {
    result_owner_ready(&test->owner);test->message=result_integer(81);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&test->owner.admission,&test->channel)==XR_XIR_VALUE_OK);
    if (mode) test->send=rendezvous_begin(&test->owner,&test->channel,&test->message);
    if (mode>=2) test->other=rendezvous_begin(&test->owner,&test->channel,&test->message);
    if (mode==3) {
        XirChannel *storage=(XirChannel *)object_pointer(&test->channel);atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
        xr_xir_channel_send_wait_drop(test->send);test->send=NULL;
        atomic_store_explicit(&storage->locked,false,memory_order_release);
    }
    XrXirChannelWatchState state=XR_XIR_CHANNEL_WATCH_RETRY;
    CHECK(xr_xir_channel_watch_begin(&test->channel,channel_call_sequence(&test->owner,&test->channel),
        &test->owner.admission,&test->watch,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_WATCH_PENDING);
}
static XrXirValueStatus rendezvous_work_operation(RendezvousWork *test,unsigned mode,
    XrXirChannelSendWait **created,XrXirChannelSendWaitState *waiting,XrXirChannelState *state) {
    if (mode==0 || mode==3) return xr_xir_channel_rendezvous_send_begin(&test->channel,&test->message,&test->owner.admission,created,waiting);
    if (mode==1) return xr_xir_channel_receive_result(&test->channel,(XrXirType)RESULT_I64,&test->owner.admission,&test->output,state);
    return xr_xir_channel_close(&test->channel,&test->owner.admission,state);
}
static void rendezvous_work_drop(RendezvousWork *test,XrXirChannelSendWait *created) {
    xr_xir_channel_watch_drop(test->watch);xr_xir_channel_send_wait_drop(created);
    xr_xir_channel_send_wait_drop(test->send);xr_xir_channel_send_wait_drop(test->other);
    xr_xir_value_drop(&test->output);xr_xir_value_drop(&test->channel);result_owner_free(&test->owner);
}
static void rendezvous_work_prefixes(void) {
    uint64_t total=0;
    for (unsigned mode=0;mode<4;++mode) {
        RendezvousWork test={0};rendezvous_work_prepare(&test,mode);uint64_t begin=test.owner.admission.work;
        XrXirChannelSendWait *created=NULL;XrXirChannelSendWaitState waiting=XR_XIR_CHANNEL_SEND_WAIT_CLOSED;
        XrXirChannelState state=XR_XIR_CHANNEL_FULL;
        CHECK(rendezvous_work_operation(&test,mode,&created,&waiting,&state)==XR_XIR_VALUE_OK);
        uint64_t required=begin-test.owner.admission.work;CHECK(required && required<256);rendezvous_work_drop(&test,created);
        for (uint64_t cut=0;cut<=required;++cut) {
            test=(RendezvousWork){0};rendezvous_work_prepare(&test,mode);XirChannel *storage=(XirChannel *)object_pointer(&test.channel);
            uint64_t sequence=storage->sequence;uint32_t count=storage->send_count;size_t live=runtime_memory.live,bytes=runtime_memory.bytes;
            created=NULL;waiting=XR_XIR_CHANNEL_SEND_WAIT_CLOSED;state=XR_XIR_CHANNEL_FULL;test.owner.admission.work=cut;
            XrXirValueStatus status=rendezvous_work_operation(&test,mode,&created,&waiting,&state);
            CHECK(status==(cut==required ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT));
            if (cut<required) {
                CHECK(!created && waiting==XR_XIR_CHANNEL_SEND_WAIT_CLOSED && state==XR_XIR_CHANNEL_FULL && unit_value(&test.output));
                CHECK(storage->sequence==sequence && storage->send_count==count && !atomic_load_explicit(&storage->closed,memory_order_acquire));
                CHECK(!xr_xir_channel_watch_ready(test.watch) && runtime_memory.live==live && runtime_memory.bytes==bytes);
                if (test.send) CHECK(xr_xir_channel_send_wait_state(test.send)==XR_XIR_CHANNEL_SEND_WAIT_PENDING);
                if (test.other) CHECK(xr_xir_channel_send_wait_state(test.other)==XR_XIR_CHANNEL_SEND_WAIT_PENDING);
            } else CHECK(xr_xir_channel_watch_ready(test.watch));
            test.owner.admission.work=128000000;rendezvous_work_drop(&test,created);
        }
        total+=required+1;printf("Rendezvous work mode=%u exact=%" PRIu64 " all_prefixes=%" PRIu64 "\n",mode,required,required+1);
    }
    printf("Rendezvous complete local work prefixes=%" PRIu64 " preserve payload, queue, prepared result, ack and observers\n",total);
}
static void rendezvous_retired_domain(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(1);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    _Alignas(8) unsigned char prefix[8]={0x58,0x52,0x44,0x4d,0x1a,0,0,1};
    XrXirValueAdmission admission=owner.admission;admission.domain=(XrXirDomain *)(void *)prefix;
    size_t begin=runtime_memory.attempts;XrXirChannelSendWait *send=NULL;XrXirChannelSendWaitState state=XR_XIR_CHANNEL_SEND_WAIT_SENT;
    CHECK(xr_xir_channel_rendezvous_send_begin(&channel,&message,&admission,&send,&state)==XR_XIR_VALUE_BAD_ARGUMENT &&
        !send && state==XR_XIR_CHANNEL_SEND_WAIT_SENT && runtime_memory.attempts==begin);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void rendezvous_reference_limits(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message={0},output={0};
    rendezvous_fault_message(&owner,true,&channel,&message);XrXirChannelSendWait *send=rendezvous_begin(&owner,&channel,&message);
    XirChannel *storage=(XirChannel *)object_pointer(&channel);uint64_t sequence=storage->sequence;
    size_t live=runtime_memory.live,bytes=runtime_memory.bytes;XrXirChannelState state=XR_XIR_CHANNEL_FULL;
    atomic_store_explicit(&send->references,UINT32_MAX,memory_order_relaxed);
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_ARRAY_I64,&owner.admission,&output,&state)==XR_XIR_VALUE_REFCOUNT_LIMIT &&
        unit_value(&output) && state==XR_XIR_CHANNEL_FULL && storage->send_count==1 && storage->sequence==sequence);
    atomic_store_explicit(&send->references,2,memory_order_relaxed);
    XirObject *payload=object_pointer(&message);uint32_t references=atomic_load_explicit(&payload->references,memory_order_relaxed);
    atomic_store_explicit(&payload->references,UINT32_MAX,memory_order_relaxed);
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_ARRAY_I64,&owner.admission,&output,&state)==XR_XIR_VALUE_REFCOUNT_LIMIT &&
        unit_value(&output) && state==XR_XIR_CHANNEL_FULL && atomic_load_explicit(&send->references,memory_order_relaxed)==2);
    atomic_store_explicit(&payload->references,references,memory_order_relaxed);
    uint32_t domains=atomic_load_explicit(&owner.domain->references,memory_order_relaxed);
    atomic_store_explicit(&owner.domain->references,UINT32_MAX,memory_order_relaxed);
    XrXirChannelSendWait *other=NULL;XrXirChannelSendWaitState waiting=XR_XIR_CHANNEL_SEND_WAIT_CLOSED;
    CHECK(xr_xir_channel_rendezvous_send_begin(&channel,&message,&owner.admission,&other,&waiting)==XR_XIR_VALUE_REFCOUNT_LIMIT &&
        !other && waiting==XR_XIR_CHANNEL_SEND_WAIT_CLOSED && storage->sequence==sequence && storage->send_count==1);
    atomic_store_explicit(&owner.domain->references,domains,memory_order_relaxed);
    CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_ARRAY_I64,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    xr_xir_value_drop(&output);xr_xir_channel_send_wait_drop(send);xr_xir_value_drop(&message);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void rendezvous_fault_cases(void) {
    rendezvous_begin_faults(false);rendezvous_begin_faults(true);rendezvous_receive_faults(false);rendezvous_receive_faults(true);
    rendezvous_work_prefixes();rendezvous_retired_domain();rendezvous_reference_limits();
}
#endif // XIR_CHANNEL_RENDEZVOUS_FAULT_CASES_H
