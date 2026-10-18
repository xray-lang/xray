/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_recv_wait_thread_cases.h - Actual sending CAS versus cancellation
 */
#ifndef XIR_CHANNEL_RECV_WAIT_THREAD_CASES_H
#define XIR_CHANNEL_RECV_WAIT_THREAD_CASES_H
static void *recv_wait_thread_send(void *context) {
    RendezvousThread *thread=context;XrXirValue message=result_integer(917);
    thread->status=xr_xir_channel_try_send(&thread->channel,&message,(XrXirType)RESULT_SEND,&thread->admission,&thread->output,&thread->state);
    CHECK(message.payload==917);xr_xir_value_drop(&thread->channel);return NULL;
}
static void recv_wait_thread_cancel_case(bool withdraw) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelRecvWait *recv=recv_wait_begin(&owner,&channel,(XrXirType)RESULT_I64);XirChannel *storage=(XirChannel *)object_pointer(&channel);
    RendezvousThread context={.admission=owner.admission,.state=XR_XIR_CHANNEL_EMPTY};atomic_init(&context.phase,0);
    CHECK(xr_xir_value_copy(&channel,&context.channel)==XR_XIR_VALUE_OK);rendezvous_paused_thread=&context;
    channel_cas32_target=&recv->state;channel_cas32_count=0;channel_cas32_pause_at=1;channel_cas32_before=rendezvous_pause_receive;
    xr_thread_t thread;CHECK(xr_thread_create(&thread,recv_wait_thread_send,&context));
    unsigned attempts=0;while (atomic_load_explicit(&context.phase,memory_order_acquire)!=1) { CHECK(++attempts<1000000);xr_thread_yield(); }
    CHECK(atomic_load_explicit(&storage->locked,memory_order_relaxed));
    size_t before=runtime_memory.attempts;
    if (withdraw) CHECK(xr_xir_channel_recv_wait_cancel_pending(recv) && xr_xir_channel_recv_wait_owner(recv,&owner.admission));
    else xr_xir_channel_recv_wait_drop(recv);
    CHECK(runtime_memory.attempts==before);atomic_store_explicit(&context.phase,2,memory_order_release);
    CHECK(xr_thread_join(thread,NULL)==0 && context.status==XR_XIR_VALUE_OK && context.state==XR_XIR_CHANNEL_RETRY && unit_value(&context.output));
    channel_cas32_target=NULL;rendezvous_paused_thread=NULL;
    CHECK(storage->recv_count==1 && (withdraw ? xr_xir_value_valid(&storage->recv_head->owner_channel) : unit_value(&storage->recv_head->owner_channel)) &&
        storage->recv_head->reservation && !storage->recv_head->reservation->object.graph_registered);
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK && !storage->recv_count);
    if (withdraw) xr_xir_channel_recv_wait_drop(recv);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
    puts("Receive-first actual OS sender pauses at commit CAS; held-lock cancellation wins without ack or result publication, physical0");
}
static void recv_wait_thread_cancel(void) { recv_wait_thread_cancel_case(false);recv_wait_thread_cancel_case(true); }
#endif // XIR_CHANNEL_RECV_WAIT_THREAD_CASES_H
