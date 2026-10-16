/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_recv_wait_task_cases.h - Canonical executor receive ownership
 */
#ifndef XIR_CHANNEL_RECV_WAIT_TASK_CASES_H
#define XIR_CHANNEL_RECV_WAIT_TASK_CASES_H
static void recv_wait_task_pair(bool closed) {
    RendezvousTask f={0};rendezvous_task_new_mode(&f,true);XrXirValue received=rendezvous_task_spawn(&f,2),sent={0};
    CHECK(xr_xir_task_executor_poll(f.executor,1)==XR_XIR_CALL_SUSPENDED && f.received.callbacks==1);
    for (unsigned i=0;i<16;++i) CHECK(xr_xir_task_executor_poll(f.executor,1)==XR_XIR_CALL_SUSPENDED && f.received.callbacks==1);
    if (closed) { XrXirChannelState state=XR_XIR_CHANNEL_RETRY;CHECK(xr_xir_channel_close(&f.channel,&f.owner.admission,&state)==XR_XIR_VALUE_OK); }
    else sent=rendezvous_task_spawn(&f,1);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for (unsigned i=0;i<32 && status==XR_XIR_CALL_READY;++i) status=xr_xir_task_executor_poll(f.executor,4);
    CHECK(status==XR_XIR_CALL_RETURNED && f.received.releases==1 && (closed || f.sent.releases==1));
    XrXirCallResult result={0};CHECK(xr_xir_task_copy_outcome(&received,&result)==XR_XIR_CALL_RETURNED && result.value.payload==(closed ? -3 : 701));
    xr_xir_call_result_drop(&result);
    if (!closed) { CHECK(xr_xir_task_copy_outcome(&sent,&result)==XR_XIR_CALL_RETURNED && result.value.payload==1);xr_xir_call_result_drop(&result); }
    xr_xir_value_drop(&received);xr_xir_value_drop(&sent);rendezvous_task_drop(&f);
}
static void recv_wait_task_stop(void) {
    RendezvousTask f={0};rendezvous_task_new_mode(&f,true);XrXirValue received=rendezvous_task_spawn(&f,2);
    CHECK(xr_xir_task_executor_poll(f.executor,1)==XR_XIR_CALL_SUSPENDED);
    XirChannel *storage=(XirChannel *)object_pointer(&f.channel);atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    CHECK(xr_xir_task_executor_stop(f.executor)==XR_XIR_CALL_CANCEL_REQUESTED);XrXirCallStatus status=XR_XIR_CALL_READY;
    for (unsigned i=0;i<16 && status==XR_XIR_CALL_READY;++i) status=xr_xir_task_executor_poll(f.executor,1);
    CHECK(status==XR_XIR_CALL_RETURNED && f.received.releases==1 && f.received.reason==XR_XIR_CALL_CANCELLED);
    XrXirCallResult result={0};CHECK(xr_xir_task_copy_outcome(&received,&result)==XR_XIR_CALL_CANCELLED);xr_xir_call_result_drop(&result);xr_xir_value_drop(&received);
    atomic_store_explicit(&storage->locked,false,memory_order_release);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_close(&f.channel,&f.owner.admission,&state)==XR_XIR_VALUE_OK && !storage->recv_count);rendezvous_task_drop(&f);
}
static void recv_wait_task_cases(void) {
    recv_wait_task_pair(false);recv_wait_task_pair(true);recv_wait_task_stop();
    puts("Receive-first actual native Task executor: owned receiver, real sender701 or Closed, typed terminal result, held-lock stop and exact Program code lease release physical0");
}
#endif // XIR_CHANNEL_RECV_WAIT_TASK_CASES_H
