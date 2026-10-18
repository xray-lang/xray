/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_rendezvous_thread_cases.h - True OS cancellation against a prepared receive
 */
#ifndef XIR_CHANNEL_RENDEZVOUS_THREAD_CASES_H
#define XIR_CHANNEL_RENDEZVOUS_THREAD_CASES_H
#include "os/os_thread.h"
typedef struct RendezvousThread {
    XrXirValue channel,output;
    XrXirValueAdmission admission;
    XrXirValueStatus status;
    XrXirChannelState state;
    _Atomic(uint32_t) phase;
} RendezvousThread;
static RendezvousThread *rendezvous_paused_thread;
static void rendezvous_pause_receive(void) {
    atomic_store_explicit(&rendezvous_paused_thread->phase,1,memory_order_release);
    unsigned attempts=0;
    while (atomic_load_explicit(&rendezvous_paused_thread->phase,memory_order_acquire)!=2) {
        CHECK(++attempts<1000000);xr_thread_yield();
    }
}
static void *rendezvous_thread_receive(void *context) {
    RendezvousThread *thread=context;
    thread->status=xr_xir_channel_receive_result(&thread->channel,(XrXirType)RESULT_I64,&thread->admission,&thread->output,&thread->state);
    xr_xir_value_drop(&thread->channel);return NULL;
}
static void rendezvous_thread_cancel_case(bool withdraw) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(81);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,0,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelSendWait *send=rendezvous_begin(&owner,&channel,&message);XirChannel *storage=(XirChannel *)object_pointer(&channel);
    RendezvousThread context={.admission=owner.admission,.state=XR_XIR_CHANNEL_FULL};atomic_init(&context.phase,0);
    CHECK(xr_xir_value_copy(&channel,&context.channel)==XR_XIR_VALUE_OK);rendezvous_paused_thread=&context;
    channel_cas32_target=&send->state;channel_cas32_count=0;channel_cas32_pause_at=1;channel_cas32_before=rendezvous_pause_receive;
    xr_thread_t thread;CHECK(xr_thread_create(&thread,rendezvous_thread_receive,&context));
    unsigned attempts=0;while (atomic_load_explicit(&context.phase,memory_order_acquire)!=1) { CHECK(++attempts<1000000);xr_thread_yield(); }
    CHECK(atomic_load_explicit(&storage->locked,memory_order_relaxed));
    size_t before=runtime_memory.attempts;
    if (withdraw) CHECK(xr_xir_channel_send_wait_cancel_pending(send) && xr_xir_channel_send_wait_owner(send,&owner.admission));
    else xr_xir_channel_send_wait_drop(send);
    CHECK(runtime_memory.attempts==before);atomic_store_explicit(&context.phase,2,memory_order_release);
    CHECK(xr_thread_join(thread,NULL)==0 && context.status==XR_XIR_VALUE_OK && context.state==XR_XIR_CHANNEL_RETRY && unit_value(&context.output));
    channel_cas32_target=NULL;rendezvous_paused_thread=NULL;
    CHECK(storage->send_count==1 && (withdraw ? xr_xir_value_valid(&storage->send_head->owner_channel) :
        unit_value(&storage->send_head->owner_channel)) && xr_xir_value_valid(&storage->send_head->message));
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK && !storage->send_count);
    if (withdraw) xr_xir_channel_send_wait_drop(send);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
    puts("Rendezvous actual OS receive thread pauses at commit CAS; main-thread cancellation never waits on held lock, prepared result is discarded before ack, physical0");
}
static void rendezvous_thread_cancel(void) { rendezvous_thread_cancel_case(false);rendezvous_thread_cancel_case(true); }
#endif // XIR_CHANNEL_RENDEZVOUS_THREAD_CASES_H
