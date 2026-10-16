/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_timed_call_fault_cases.h - Exhaustive owned deadline allocation failures
 */
#ifndef XIR_CHANNEL_TIMED_CALL_FAULT_CASES_H
#define XIR_CHANNEL_TIMED_CALL_FAULT_CASES_H
static XrXirCallStatus timed_channel_fault_operation(ResultOwner *owner,const XrXirValue *channel,
    XrXirChannelCallOperation operation,unsigned capacity,bool timeout) {
    bool sending=operation==XR_XIR_CHANNEL_CALL_SEND_TIMEOUT;
    if (capacity && sending) blocking_channel_host_send(owner,channel,false,81);
    TimedChannelWitness w={.operation=operation,.milliseconds=timeout ? 10 : 5000};timed_channel_provider(&w);
    XrXirCallAccounting accounting={0};XrXirCallStatus status=timed_channel_prepare(owner,channel,&w,&accounting);
    if (status==XR_XIR_CALL_READY) {
        XrXirCallResult result=xr_xir_call_poll_bounded(w.call,8);
        if (result.status==XR_XIR_CALL_SUSPENDED && !timeout) {
            if (sending) CHECK(blocking_channel_host_recv(owner,channel,false)==(capacity ? 81 : 913));
            else blocking_channel_host_send(owner,channel,false,73);
        }
        while (result.status==XR_XIR_CALL_SUSPENDED || result.status==XR_XIR_CALL_READY) {
            if (timeout) xr_time_sleep_ms(1);
            result=xr_xir_call_poll_bounded(w.call,16);
        }
        status=result.status;
        if (status==XR_XIR_CALL_RETURNED) timed_channel_check(result,timeout ? 2u : 0u,false);
    }
    timed_channel_free(&w,&accounting);blocking_channel_drain(owner,channel);
    XirChannel *storage=(XirChannel *)object_pointer(channel);
    CHECK(!storage->buffer.count && !storage->send_count && !storage->recv_count && !storage->watch_count);
    return status;
}
static void timed_channel_faults(XrXirChannelCallOperation operation,unsigned capacity,bool timeout) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,capacity,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    size_t sites=0,live=runtime_memory.live,bytes=runtime_memory.bytes;
    for (size_t ordinal=0;ordinal<=sites;++ordinal) {
        size_t begin=runtime_memory.attempts;runtime_memory.fail_at=ordinal ? begin+ordinal-1 : SIZE_MAX;
        XrXirCallStatus status=timed_channel_fault_operation(&owner,&channel,operation,capacity,timeout);
        size_t attempts=runtime_memory.attempts-begin;runtime_memory.fail_at=SIZE_MAX;
        if (!ordinal) { CHECK(status==XR_XIR_CALL_RETURNED);sites=attempts;CHECK(sites); }
        else CHECK(status==XR_XIR_CALL_OOM && attempts>=ordinal);
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
        CHECK(timed_channel_fault_operation(&owner,&channel,operation,capacity,timeout)==XR_XIR_CALL_RETURNED);
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
    }
    printf("Timed shared Call operation=%u capacity=%u timeout=%u complete runtime allocator ordinals=%zu; same Channel/Domain recovery physical baseline\n",
        operation,capacity,timeout ? 1u : 0u,sites);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void channel_timed_call_fault_cases(void) {
    for (unsigned op=XR_XIR_CHANNEL_CALL_SEND_TIMEOUT;op<=XR_XIR_CHANNEL_CALL_RECV_TIMEOUT;++op)
        for (unsigned capacity=0;capacity<2;++capacity) for (unsigned timeout=0;timeout<2;++timeout)
            timed_channel_faults((XrXirChannelCallOperation)op,capacity,timeout!=0);
}
#endif // XIR_CHANNEL_TIMED_CALL_FAULT_CASES_H
