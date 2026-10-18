/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_pending_withdraw_cases.h - First-winner intent withdrawal
 */
#ifndef XIR_CHANNEL_PENDING_WITHDRAW_CASES_H
#define XIR_CHANNEL_PENDING_WITHDRAW_CASES_H
static void channel_pending_withdraw(bool receiving,unsigned winner,bool locked,bool closed_domain) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(73),output={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XirChannel *storage=(XirChannel *)object_pointer(&channel);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    XrXirChannelSendWait *send=NULL;XrXirChannelRecvWait *recv=NULL;
    if (receiving) recv=recv_wait_begin(&owner,&channel,(XrXirType)RESULT_I64);
    else send=rendezvous_begin(&owner,&channel,&message);
    if (winner==1) {
        if (receiving) CHECK(xr_xir_channel_send_owned(&channel,&message,&owner.admission,&state)==XR_XIR_VALUE_OK &&
            state==XR_XIR_CHANNEL_SENT && unit_value(&message));
        else CHECK(rendezvous_receive(&owner,&channel,true)==73);
    } else if (winner==2) CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK);
    if (closed_domain) xr_xir_domain_close(owner.domain);
    if (locked) atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    size_t attempts=runtime_memory.attempts;
    bool won=receiving ? xr_xir_channel_recv_wait_cancel_pending(recv) : xr_xir_channel_send_wait_cancel_pending(send);
    CHECK(won==(winner==0) && runtime_memory.attempts==attempts);
    CHECK(!(receiving ? xr_xir_channel_recv_wait_cancel_pending(recv) : xr_xir_channel_send_wait_cancel_pending(send)));
    CHECK(runtime_memory.attempts==attempts && xr_xir_value_valid(&channel));
    if (receiving) CHECK(xr_xir_channel_recv_wait_state(recv)==(winner==0 ? XR_XIR_CHANNEL_RECV_WAIT_CANCELLED :
        winner==1 ? XR_XIR_CHANNEL_RECV_WAIT_VALUE : XR_XIR_CHANNEL_RECV_WAIT_CLOSED));
    else CHECK(xr_xir_channel_send_wait_state(send)==(winner==0 ? XR_XIR_CHANNEL_SEND_WAIT_CANCELLED :
        winner==1 ? XR_XIR_CHANNEL_SEND_WAIT_SENT : XR_XIR_CHANNEL_SEND_WAIT_CLOSED));
    if (locked) atomic_store_explicit(&storage->locked,false,memory_order_relaxed);
    if (!closed_domain && receiving) {
        if (winner) {
            CHECK(xr_xir_channel_recv_wait_take(recv,&output));result_variant(&output,winner==1 ? 0u : 3u,winner==1 ? 1u : 0u);
        } else CHECK(!xr_xir_channel_recv_wait_take(recv,&output) && unit_value(&output));
    }
    if (!closed_domain && !winner) {
        /* A withdrawn intent remains owned, but neither peer may acknowledge it. */
        if (receiving) CHECK(xr_xir_channel_send_owned(&channel,&message,&owner.admission,&state)==XR_XIR_VALUE_OK &&
            state==XR_XIR_CHANNEL_FULL && message.payload==73);
        else CHECK(xr_xir_channel_receive(&channel,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
            state==XR_XIR_CHANNEL_EMPTY && unit_value(&output));
        CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK);
        CHECK(!storage->send_count && !storage->recv_count);
        CHECK(receiving ? xr_xir_channel_recv_wait_state(recv)==XR_XIR_CHANNEL_RECV_WAIT_CANCELLED :
            xr_xir_channel_send_wait_state(send)==XR_XIR_CHANNEL_SEND_WAIT_CANCELLED);
    }
    xr_xir_value_drop(&output);xr_xir_channel_send_wait_drop(send);xr_xir_channel_recv_wait_drop(recv);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void channel_pending_withdraw_cases(void) {
    CHECK(!xr_xir_channel_send_wait_cancel_pending(NULL) && !xr_xir_channel_recv_wait_cancel_pending(NULL));
    for (unsigned receiving=0;receiving<2;++receiving) {
        for (unsigned winner=0;winner<3;++winner) channel_pending_withdraw(receiving!=0,winner,false,false);
        channel_pending_withdraw(receiving!=0,0,true,false);
        channel_pending_withdraw(receiving!=0,0,true,true);
    }
    puts("Owned pending intent withdrawal: peer/close/withdraw first winner, duplicate and late refusal, held-lock and closed-Domain retirement physical0");
}
#endif // XIR_CHANNEL_PENDING_WITHDRAW_CASES_H
