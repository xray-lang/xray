/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_rendezvous.c - Actual pending payloads and receive-before-ack ownership
 */
#define main channel_call_wait_reference_main
#include "test_xir_channel_call_wait.c"
#undef main
static XrXirChannelSendWait *rendezvous_begin(ResultOwner *owner,const XrXirValue *channel,const XrXirValue *message) {
    XrXirChannelSendWait *send=NULL;XrXirChannelSendWaitState state=XR_XIR_CHANNEL_SEND_WAIT_RETRY;
    CHECK(xr_xir_channel_rendezvous_send_begin(channel,message,&owner->admission,&send,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_SEND_WAIT_PENDING && send && xr_xir_channel_send_wait_state(send)==state);return send;
}
static int64_t rendezvous_receive(ResultOwner *owner,const XrXirValue *channel,bool wrapped) {
    XrXirValue result={0};XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    if (!wrapped) CHECK(xr_xir_channel_receive(channel,&owner->admission,&result,&state)==XR_XIR_VALUE_OK);
    else CHECK(xr_xir_channel_receive_result(channel,(XrXirType)RESULT_I64,&owner->admission,&result,&state)==XR_XIR_VALUE_OK);
    CHECK(state==XR_XIR_CHANNEL_VALUE);int64_t number=result.payload;
    if (wrapped) {
        XrXirEnumBorrow borrow={0};result_variant(&result,0,1);
        CHECK(xr_xir_enum_borrow(&result,&borrow)==XR_XIR_VALUE_OK);number=borrow.fields[0].payload;
    }
    xr_xir_value_drop(&result);return number;
}
static void rendezvous_fifo(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(99),sent={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelSendWait *send[8]={0};XirChannel *storage=(XirChannel *)object_pointer(&channel);
    for (unsigned i=0;i<8;++i) { XrXirValue input=result_integer(31+i);send[i]=rendezvous_begin(&owner,&channel,&input);CHECK(input.payload==31+i); }
    CHECK(!storage->buffer.capacity && !storage->buffer.count && storage->send_count==8);
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&sent,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_FULL && message.payload==99);result_variant(&sent,1,0);xr_xir_value_drop(&sent);
    for (unsigned i=0;i<8;++i) {
        CHECK(rendezvous_receive(&owner,&channel,(i&1)!=0)==31+i);
        CHECK(xr_xir_channel_send_wait_state(send[i])==XR_XIR_CHANNEL_SEND_WAIT_SENT && storage->send_count==7-i && !storage->buffer.count);
        xr_xir_channel_send_wait_drop(send[i]);
    }
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void rendezvous_close(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelSendWait *send[3]={0};
    for (unsigned i=0;i<3;++i) { XrXirValue input=result_integer(11+i);send[i]=rendezvous_begin(&owner,&channel,&input); }
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_CLOSED);
    CHECK(!((XirChannel *)object_pointer(&channel))->send_count);xr_xir_value_drop(&channel);
    for (unsigned i=0;i<3;++i) {
        CHECK(xr_xir_channel_send_wait_state(send[i])==XR_XIR_CHANNEL_SEND_WAIT_CLOSED && send[i]->message.payload==11+i);
        xr_xir_channel_send_wait_drop(send[i]);
    }
    result_owner_free(&owner);
}
static void rendezvous_cancel(bool locked,bool last,bool domain_closed) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message={0};
    CHECK(xr_xir_string_new(owner.domain,"owned",5,&message)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_STRING,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelSendWait *send=rendezvous_begin(&owner,&channel,&message);XirChannel *storage=(XirChannel *)object_pointer(&channel);
    xr_xir_value_drop(&message);if (last) xr_xir_value_drop(&channel);
    if (locked) atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    size_t attempts=runtime_memory.attempts;xr_xir_channel_send_wait_drop(send);CHECK(runtime_memory.attempts==attempts);
    if (!last) {
        CHECK(storage->send_count==(locked ? 1u : 0u));
        if (locked) CHECK(unit_value(&send->owner_channel) && xr_xir_value_valid(&send->message) &&
            atomic_load_explicit(&send->references,memory_order_relaxed)==1);
        if (domain_closed) xr_xir_domain_close(owner.domain);
        xr_xir_value_drop(&channel);
    }
    result_owner_free(&owner);
}
static void rendezvous_unit(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},unit={0},result={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_UNIT,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelSendWait *send=rendezvous_begin(&owner,&channel,&unit);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_UNIT,&owner.admission,&result,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    XrXirEnumBorrow borrow={0};CHECK(xr_xir_enum_borrow(&result,&borrow)==XR_XIR_VALUE_OK && borrow.field_count==1 && unit_value(&borrow.fields[0]));
    CHECK(xr_xir_channel_send_wait_state(send)==XR_XIR_CHANNEL_SEND_WAIT_SENT);
    xr_xir_channel_send_wait_drop(send);xr_xir_value_drop(&result);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void rendezvous_cow(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},array={0},result={0};
    XrXirValue fields[]={result_integer(11),result_integer(22)};
    CHECK(xr_xir_array_new((XrXirType)RESULT_ARRAY,fields,2,&owner.admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_ARRAY,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelSendWait *send=rendezvous_begin(&owner,&channel,&array);
    XrXirValue replacement=result_integer(99);XrXirValuePlace place={(XrXirType)RESULT_ARRAY,&array.payload};XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_set(&place,0,&replacement,&owner.admission,&fault)==XR_XIR_VALUE_OK);
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_ARRAY_I64,&owner.admission,&result,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    XrXirEnumBorrow borrow={0};CHECK(xr_xir_enum_borrow(&result,&borrow)==XR_XIR_VALUE_OK);
    const XirArray *before=(const XirArray *)object_pointer(&borrow.fields[0]),*after=(const XirArray *)object_pointer(&array);
    int64_t first=0,second=0;memcpy(&first,before->data,8);memcpy(&second,after->data,8);CHECK(first==11 && second==99);
    CHECK(xr_xir_channel_send_wait_state(send)==XR_XIR_CHANNEL_SEND_WAIT_SENT);xr_xir_channel_send_wait_drop(send);
    xr_xir_value_drop(&result);xr_xir_value_drop(&array);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void rendezvous_nested_identity(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},inner={0},result={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_STRING,1,&owner.admission,&inner)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_NESTED,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelSendWait *send=rendezvous_begin(&owner,&channel,&inner);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_NESTED,&owner.admission,&result,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    XrXirEnumBorrow borrow={0};CHECK(xr_xir_enum_borrow(&result,&borrow)==XR_XIR_VALUE_OK && borrow.fields[0].payload==inner.payload);
    CHECK(xr_xir_channel_close(&borrow.fields[0],&owner.admission,&state)==XR_XIR_VALUE_OK);
    int64_t capacity=0;bool closed=false;CHECK(xr_xir_channel_info(&inner,&owner.admission,&capacity,&closed)==XR_XIR_VALUE_OK && closed && capacity==1);
    xr_xir_channel_send_wait_drop(send);xr_xir_value_drop(&result);xr_xir_value_drop(&inner);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static XrXirChannelSendWait *rendezvous_race_sender;
static void rendezvous_cancel_frontier(void) { xr_xir_channel_send_wait_drop(rendezvous_race_sender);rendezvous_race_sender=NULL; }
static void rendezvous_cancel_race(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},input=result_integer(71),output={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    rendezvous_race_sender=rendezvous_begin(&owner,&channel,&input);XirChannel *storage=(XirChannel *)object_pointer(&channel);
    channel_cas_target=&storage->locked;channel_cas_count=0;channel_cas_pause_at=2;channel_cas_before=rendezvous_cancel_frontier;
    XrXirChannelState state=XR_XIR_CHANNEL_FULL;
    CHECK(xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_I64,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_RETRY && unit_value(&output) && !rendezvous_race_sender && !channel_cas_before && !storage->send_count);
    channel_cas_target=NULL;xr_xir_value_drop(&channel);result_owner_free(&owner);
}
#include "xir_channel_rendezvous_fault_cases.h"
#include "xir_channel_rendezvous_call_cases.h"
#include "xir_channel_rendezvous_thread_cases.h"
#include "xir_channel_recv_wait_cases.h"
#include "xir_channel_recv_wait_fault_cases.h"
#include "xir_channel_recv_wait_thread_cases.h"
#include "xir_channel_recv_wait_call_cases.h"
#include "xir_channel_rendezvous_task_cases.h"
#include "xir_channel_recv_wait_task_cases.h"
#include "xir_channel_recv_raw_cases.h"
#include "xir_channel_inbox_move_cases.h"
#include "xir_channel_blocking_call_cases.h"
#include "xir_channel_pending_withdraw_cases.h"
#include "xir_channel_timed_call_cases.h"
#include "xir_channel_timed_call_fault_cases.h"
int main(int argc,char **argv) {
    _Static_assert(XR_XIR_VALUE_ABI_VERSION==29 && XR_XIR_CALL_ABI_VERSION==33 && XR_XIR_PROGRAM_ABI_VERSION==35,
        "Owned pending payloads require the exact current shared storage generation");
    if (argc==2 && !strcmp(argv[1],"--timed-normal")) { channel_timed_call_cases();return 0; }
    if (argc==2 && !strcmp(argv[1],"--timed-resources")) { channel_timed_call_fault_cases();return 0; }
    CHECK(argc==1);
    rendezvous_fifo();rendezvous_close();rendezvous_unit();rendezvous_cow();rendezvous_nested_identity();rendezvous_cancel_race();
    for (unsigned lock=0;lock<2;++lock) for (unsigned last=0;last<2;++last) rendezvous_cancel(lock!=0,last!=0,false);
    rendezvous_cancel(true,false,true);
    rendezvous_fault_cases();
    rendezvous_call_cases();rendezvous_call_faults();rendezvous_thread_cancel();
    rendezvous_call_owner(false);rendezvous_call_owner(true);rendezvous_call_staged_rejection();rendezvous_task_cases();
    recv_wait_cases();recv_wait_fault_cases();recv_wait_thread_cancel();recv_wait_call_cases();recv_wait_task_cases();
    recv_raw_cases();
    channel_inbox_move_cases();
    blocking_channel_cases();
    channel_pending_withdraw_cases();
    channel_timed_call_cases();
    channel_timed_call_fault_cases();
    puts("Channel zero-capacity pending sends: FIFO, typed receive-before-ack, Unit/COW, close and nonblocking cancellation physical0");return 0;
}
