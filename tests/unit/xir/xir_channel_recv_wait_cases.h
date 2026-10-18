/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_recv_wait_cases.h - Native receive reservations and physical owners
 */
#ifndef XIR_CHANNEL_RECV_WAIT_CASES_H
#define XIR_CHANNEL_RECV_WAIT_CASES_H
static XrXirChannelRecvWait *recv_wait_begin(ResultOwner *owner,const XrXirValue *channel,XrXirType type) {
    XrXirChannelRecvWait *recv=NULL;XrXirChannelRecvWaitState state=XR_XIR_CHANNEL_RECV_WAIT_RETRY;
    CHECK(xr_xir_channel_rendezvous_recv_begin(channel,type,&owner->admission,&recv,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_RECV_WAIT_PENDING && recv && xr_xir_channel_recv_wait_state(recv)==state);
    CHECK(recv->reservation && !recv->reservation->object.graph_registered && unit_value(&recv->result));return recv;
}
static XrXirValue recv_wait_take(XrXirChannelRecvWait *recv,uint32_t variant) {
    XrXirValue output={0};size_t attempts=runtime_memory.attempts;
    CHECK(xr_xir_channel_recv_wait_take(recv,&output));result_variant(&output,variant,variant ? 0u : 1u);
    XrXirValue again={0};CHECK(!xr_xir_channel_recv_wait_take(recv,&again) && unit_value(&again) && runtime_memory.attempts==attempts);
    xr_xir_channel_recv_wait_drop(recv);CHECK(runtime_memory.attempts==attempts);return output;
}
static void recv_wait_fifo(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},result={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XirChannel *storage=(XirChannel *)object_pointer(&channel);XrXirChannelRecvWait *recv[8]={0};
    for (unsigned i=0;i<8;++i) recv[i]=recv_wait_begin(&owner,&channel,(XrXirType)RESULT_I64);
    CHECK(storage->recv_count==8 && !storage->buffer.count && !storage->buffer.capacity && !storage->send_count);
    CHECK(!xr_xir_channel_recv_wait_take(recv[0],&result) && unit_value(&result));
    XrXirValue message=result_integer(1);XrXirChannelSendWait *send=NULL;XrXirChannelSendWaitState waiting=XR_XIR_CHANNEL_SEND_WAIT_CLOSED;
    CHECK(xr_xir_channel_rendezvous_send_begin(&channel,&message,&owner.admission,&send,&waiting)==XR_XIR_VALUE_OK &&
        !send && waiting==XR_XIR_CHANNEL_SEND_WAIT_RETRY && !storage->send_count && storage->recv_count==8);
    for (unsigned i=0;i<8;++i) {
        message=result_integer(101+i);XrXirChannelState state=XR_XIR_CHANNEL_EMPTY;
        CHECK(xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&result,&state)==XR_XIR_VALUE_OK &&
            state==XR_XIR_CHANNEL_SENT && message.payload==101+i);result_variant(&result,0,0);xr_xir_value_drop(&result);
        CHECK(xr_xir_channel_recv_wait_state(recv[i])==XR_XIR_CHANNEL_RECV_WAIT_VALUE && !storage->buffer.count && storage->recv_count==7-i);
        result=recv_wait_take(recv[i],0);XrXirEnumBorrow borrow={0};CHECK(xr_xir_enum_borrow(&result,&borrow)==XR_XIR_VALUE_OK && borrow.fields[0].payload==101+i);
        xr_xir_value_drop(&result);
    }
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void recv_wait_close(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelRecvWait *recv[3]={0};for (unsigned i=0;i<3;++i) recv[i]=recv_wait_begin(&owner,&channel,(XrXirType)RESULT_I64);
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;size_t attempts=runtime_memory.attempts;
    CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_CLOSED && runtime_memory.attempts==attempts);
    XrXirChannelRecvWait *other=NULL;XrXirChannelRecvWaitState waiting=XR_XIR_CHANNEL_RECV_WAIT_VALUE;
    CHECK(xr_xir_channel_rendezvous_recv_begin(&channel,(XrXirType)RESULT_I64,&owner.admission,&other,&waiting)==XR_XIR_VALUE_OK &&
        !other && waiting==XR_XIR_CHANNEL_RECV_WAIT_CLOSED);
    xr_xir_value_drop(&channel);
    for (unsigned i=0;i<3;++i) {
        CHECK(xr_xir_channel_recv_wait_state(recv[i])==XR_XIR_CHANNEL_RECV_WAIT_CLOSED);
        XrXirValue result=recv_wait_take(recv[i],3);xr_xir_value_drop(&result);
    }
    result_owner_free(&owner);
}
static void recv_wait_cancel(bool locked,bool last,bool closed_domain) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelRecvWait *recv=recv_wait_begin(&owner,&channel,(XrXirType)RESULT_I64);XirChannel *storage=(XirChannel *)object_pointer(&channel);
    if (last) xr_xir_value_drop(&channel);
    if (locked) atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    size_t attempts=runtime_memory.attempts;xr_xir_channel_recv_wait_drop(recv);CHECK(runtime_memory.attempts==attempts);
    if (!last) {
        CHECK(storage->recv_count==(locked ? 1u : 0u));
        if (locked) CHECK(unit_value(&recv->owner_channel) && !recv->reservation->object.graph_registered);
        if (closed_domain) xr_xir_domain_close(owner.domain);
        xr_xir_value_drop(&channel);
    }
    result_owner_free(&owner);
}
static void recv_wait_typed(unsigned kind) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message={0},sent={0},inner={0};
    XrXirType channel_types[]={(XrXirType)RESULT_CHANNEL_UNIT,(XrXirType)RESULT_CHANNEL_STRING,(XrXirType)RESULT_CHANNEL_ARRAY,(XrXirType)RESULT_CHANNEL_NESTED};
    XrXirType result_types[]={(XrXirType)RESULT_UNIT,(XrXirType)RESULT_STRING,(XrXirType)RESULT_ARRAY_I64,(XrXirType)RESULT_NESTED};
    if (kind==1) CHECK(xr_xir_string_new(owner.domain,"reservation",11,&message)==XR_XIR_VALUE_OK);
    if (kind==2) { XrXirValue field=result_integer(11);CHECK(xr_xir_array_new((XrXirType)RESULT_ARRAY,&field,1,&owner.admission,&message)==XR_XIR_VALUE_OK); }
    if (kind==3) {
        CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_STRING,1,&owner.admission,&message)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_value_copy(&message,&inner)==XR_XIR_VALUE_OK);
    }
    CHECK(xr_xir_channel_new(channel_types[kind],0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelRecvWait *recv=recv_wait_begin(&owner,&channel,result_types[kind]);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&sent,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT);
    if (kind==2) {
        XrXirValue replacement=result_integer(99);XrXirValuePlace place={(XrXirType)RESULT_ARRAY,&message.payload};XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_set(&place,0,&replacement,&owner.admission,&fault)==XR_XIR_VALUE_OK);
    }
    xr_xir_value_drop(&message);xr_xir_value_drop(&channel);xr_xir_value_drop(&sent);XrXirValue result=recv_wait_take(recv,0);XrXirEnumBorrow borrow={0};
    CHECK(xr_xir_enum_borrow(&result,&borrow)==XR_XIR_VALUE_OK);
    if (kind==0) CHECK(unit_value(&borrow.fields[0]));
    if (kind==1) { const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&borrow.fields[0],&bytes,&length) && length==11 && !memcmp(bytes,"reservation",11)); }
    if (kind==2) { const XirArray *array=(const XirArray *)object_pointer(&borrow.fields[0]);int64_t number=0;memcpy(&number,array->data,8);CHECK(number==11); }
    if (kind==3) {
        CHECK(inner.payload==borrow.fields[0].payload && xr_xir_channel_close(&borrow.fields[0],&owner.admission,&state)==XR_XIR_VALUE_OK);
        int64_t capacity=0;bool closed=false;CHECK(xr_xir_channel_info(&inner,&owner.admission,&capacity,&closed)==XR_XIR_VALUE_OK && closed && capacity==1);
    }
    xr_xir_value_drop(&inner);xr_xir_value_drop(&result);result_owner_free(&owner);
}
static void recv_wait_cross_domain(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(219),result={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirDomain *other=NULL;XrXirDomainBudgetControls controls={8388608,8388608,128000000,4194304,4194304};
    CHECK(xr_xir_domain_new_budgeted(8388608,&controls,&other)==XR_XIR_VALUE_OK);
    XrXirValueAdmission receiver=owner.admission;receiver.domain=other;
    XrXirChannelRecvWait *recv=NULL;XrXirChannelRecvWaitState waiting=XR_XIR_CHANNEL_RECV_WAIT_RETRY;
    CHECK(xr_xir_channel_rendezvous_recv_begin(&channel,(XrXirType)RESULT_I64,&receiver,&recv,&waiting)==XR_XIR_VALUE_OK && recv);
    CHECK(xr_xir_channel_recv_wait_owner(recv,&receiver) && !xr_xir_channel_recv_wait_owner(recv,&owner.admission));
    receiver=(XrXirValueAdmission){0};xr_xir_domain_close(other);xr_xir_domain_drop(other);
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_send_owned(&channel,&message,&owner.admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT && unit_value(&message));
    result=recv_wait_take(recv,0);XrXirEnumBorrow borrow={0};CHECK(xr_xir_enum_borrow(&result,&borrow)==XR_XIR_VALUE_OK && borrow.fields[0].payload==219);
    xr_xir_value_drop(&result);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void recv_wait_existing_sender(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(67);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelSendWait *send=rendezvous_begin(&owner,&channel,&message);XrXirChannelRecvWait *recv=NULL;
    XrXirChannelRecvWaitState state=XR_XIR_CHANNEL_RECV_WAIT_VALUE;XirChannel *storage=(XirChannel *)object_pointer(&channel);
    CHECK(xr_xir_channel_rendezvous_recv_begin(&channel,(XrXirType)RESULT_I64,&owner.admission,&recv,&state)==XR_XIR_VALUE_OK &&
        !recv && state==XR_XIR_CHANNEL_RECV_WAIT_RETRY && storage->send_count==1 && !storage->recv_count);
    CHECK(rendezvous_receive(&owner,&channel,true)==67);xr_xir_channel_send_wait_drop(send);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void recv_wait_cases(void) {
    recv_wait_fifo();recv_wait_close();recv_wait_cross_domain();recv_wait_existing_sender();
    for (unsigned i=0;i<4;++i) recv_wait_typed(i);
    for (unsigned lock=0;lock<2;++lock) for (unsigned last=0;last<2;++last) recv_wait_cancel(lock!=0,last!=0,false);
    recv_wait_cancel(true,false,true);
    puts("Receive-first native reservations: FIFO8, actual Recv, Unit/String/Array COW/nested Channel, cross-Domain after admission death, close and nonblocking cancellation physical0");
}
#endif // XIR_CHANNEL_RECV_WAIT_CASES_H
