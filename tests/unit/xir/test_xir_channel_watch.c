/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_watch.c - Actual readiness events, owned cancellation and failure preservation
 */
#define main channel_result_reference_main
#include "test_xir_channel_results.c"
#undef main
#include "xir/xxir_channel_watch_internal.h"
static uint64_t watch_sequence(ResultOwner *owner,const XrXirValue *channel) {
    XrXirChannelObservation observation={0};XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_observe(channel,&owner->admission,&observation,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    CHECK(observation.capacity==1 && !observation.closed);return observation.sequence;
}
static XrXirChannelWatch *watch_new(ResultOwner *owner,const XrXirValue *channel) {
    uint64_t sequence=watch_sequence(owner,channel);XrXirChannelWatch *watch=NULL;XrXirChannelWatchState state=XR_XIR_CHANNEL_WATCH_RETRY;
    CHECK(xr_xir_channel_watch_begin(channel,sequence,&owner->admission,&watch,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_WATCH_PENDING && watch && !xr_xir_channel_watch_ready(watch));return watch;
}
static void watch_events(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(81),output={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelWatch *first=watch_new(&owner,&channel),*second=watch_new(&owner,&channel);
    XirChannel *storage=(XirChannel *)object_pointer(&channel);CHECK(storage->watch_count==2);
    result_send(&owner,&channel,&message);CHECK(xr_xir_channel_watch_ready(first) && xr_xir_channel_watch_ready(second) && !storage->watch_count);
    xr_xir_channel_watch_drop(first);xr_xir_channel_watch_drop(second);
    first=watch_new(&owner,&channel);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_receive(&channel,&owner.admission,&output,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE && output.payload==81);
    CHECK(xr_xir_channel_watch_ready(first));xr_xir_value_drop(&output);xr_xir_channel_watch_drop(first);
    first=watch_new(&owner,&channel);CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_channel_watch_ready(first));xr_xir_channel_watch_drop(first);
    size_t attempts=runtime_memory.attempts;first=NULL;XrXirChannelWatchState watched=XR_XIR_CHANNEL_WATCH_PENDING;
    CHECK(xr_xir_channel_watch_begin(&channel,storage->sequence,&owner.admission,&first,&watched)==XR_XIR_VALUE_OK &&
        watched==XR_XIR_CHANNEL_WATCH_READY && !first && runtime_memory.attempts==attempts);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void watch_cancel(bool last_owner) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelWatch *watch=watch_new(&owner,&channel);XirChannel *storage=(XirChannel *)object_pointer(&channel);
    if (last_owner) xr_xir_value_drop(&channel);
    atomic_store_explicit(&storage->locked,true,memory_order_relaxed);size_t attempts=runtime_memory.attempts;
    xr_xir_channel_watch_drop(watch);CHECK(runtime_memory.attempts==attempts);
    if (!last_owner) {
        CHECK(storage->watch_count==1 && storage->watch_head==watch && !watch->owner_channel.type &&
            atomic_load_explicit(&watch->references,memory_order_relaxed)==1 &&
            atomic_load_explicit(&watch->state,memory_order_acquire)==CHANNEL_WATCH_CANCELLED);
        atomic_store_explicit(&storage->locked,false,memory_order_release);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
        CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK && !storage->watch_count);
        xr_xir_value_drop(&channel);
    }
    result_owner_free(&owner);
}
static void watch_owner_root(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelWatch *watch=watch_new(&owner,&channel);xr_xir_value_drop(&channel);
    xr_xir_domain_close(owner.domain);CHECK(xr_xir_value_valid(&watch->owner_channel) && !xr_xir_channel_watch_ready(watch));
    xr_xir_channel_watch_drop(watch);result_owner_free(&owner);
}
static ResultOwner *watch_race_owner;static const XrXirValue *watch_race_channel;
static void watch_race_publish(void) { XrXirValue message=result_integer(93);result_send(watch_race_owner,watch_race_channel,&message); }
static void watch_changed_frontier(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    uint64_t sequence=watch_sequence(&owner,&channel);watch_race_owner=&owner;watch_race_channel=&channel;
    XrXirChannelWatch *watch=NULL;XrXirChannelWatchState state=XR_XIR_CHANNEL_WATCH_PENDING;
    size_t live=runtime_memory.live,bytes=runtime_memory.bytes;XirChannel *storage=(XirChannel *)object_pointer(&channel);
    channel_cas_target=&storage->locked;channel_cas_count=0;channel_cas_pause_at=2;channel_cas_before=watch_race_publish;
    CHECK(xr_xir_channel_watch_begin(&channel,sequence,&owner.admission,&watch,&state)==XR_XIR_VALUE_OK &&
        !channel_cas_before && state==XR_XIR_CHANNEL_WATCH_READY && !watch);
    CHECK(storage->buffer.count==1 && !storage->watch_count);channel_cas_target=NULL;
    CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
    xr_xir_value_drop(&channel);result_owner_free(&owner);watch_race_owner=NULL;watch_race_channel=NULL;
}
static void watch_failures(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    uint64_t sequence=watch_sequence(&owner,&channel);XirChannel *storage=(XirChannel *)object_pointer(&channel);
    size_t live=runtime_memory.live,bytes=runtime_memory.bytes,begin=runtime_memory.attempts;
    XrXirChannelWatch *watch=NULL;XrXirChannelWatchState state=XR_XIR_CHANNEL_WATCH_READY;
    runtime_memory.fail_at=begin;
    CHECK(xr_xir_channel_watch_begin(&channel,sequence,&owner.admission,&watch,&state)==XR_XIR_VALUE_OOM &&
        !watch && state==XR_XIR_CHANNEL_WATCH_READY && runtime_memory.attempts==begin+1 && !storage->watch_count);
    runtime_memory.fail_at=SIZE_MAX;CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
    CHECK(xr_xir_channel_watch_begin(&channel,sequence,&owner.admission,&watch,&state)==XR_XIR_VALUE_OK && watch);
    xr_xir_channel_watch_drop(watch);CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
    uint32_t references=atomic_load_explicit(&owner.domain->references,memory_order_relaxed);
    atomic_store_explicit(&owner.domain->references,UINT32_MAX,memory_order_relaxed);watch=NULL;state=XR_XIR_CHANNEL_WATCH_READY;
    CHECK(xr_xir_channel_watch_begin(&channel,sequence,&owner.admission,&watch,&state)==XR_XIR_VALUE_REFCOUNT_LIMIT && !watch && !storage->watch_count);
    atomic_store_explicit(&owner.domain->references,references,memory_order_relaxed);
    atomic_store_explicit(&storage->locked,true,memory_order_relaxed);begin=runtime_memory.attempts;
    CHECK(xr_xir_channel_watch_begin(&channel,sequence,&owner.admission,&watch,&state)==XR_XIR_VALUE_OK &&
        !watch && state==XR_XIR_CHANNEL_WATCH_RETRY && runtime_memory.attempts==begin);
    XrXirChannelObservation observation={UINT64_MAX,17,13,true};XrXirChannelState observed=XR_XIR_CHANNEL_VALUE;
    CHECK(xr_xir_channel_observe(&channel,&owner.admission,&observation,&observed)==XR_XIR_VALUE_OK &&
        observed==XR_XIR_CHANNEL_RETRY && observation.sequence==UINT64_MAX && observation.count==13);
    atomic_store_explicit(&storage->locked,false,memory_order_release);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void watch_work_prefixes(void) {
    for (uint64_t cut=0;cut<=55;++cut) {
        ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(97),output={0};
        CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
        XrXirChannelWatch *first=watch_new(&owner,&channel),*second=watch_new(&owner,&channel);owner.admission.work=cut;
        XrXirChannelState state=XR_XIR_CHANNEL_VALUE;
        XrXirValueStatus status=xr_xir_channel_try_send(&channel,&message,(XrXirType)RESULT_SEND,&owner.admission,&output,&state);
        CHECK(status==(cut==55 ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT) && message.payload==97);
        XirChannel *storage=(XirChannel *)object_pointer(&channel);
        if (cut<55) CHECK(result_empty(&output) && state==XR_XIR_CHANNEL_VALUE && !storage->buffer.count &&
            storage->watch_count==2 && !xr_xir_channel_watch_ready(first) && !xr_xir_channel_watch_ready(second));
        else CHECK(state==XR_XIR_CHANNEL_SENT && storage->buffer.count==1 && !storage->watch_count &&
            xr_xir_channel_watch_ready(first) && xr_xir_channel_watch_ready(second));
        xr_xir_channel_watch_drop(first);xr_xir_channel_watch_drop(second);xr_xir_value_drop(&output);
        xr_xir_value_drop(&channel);result_owner_free(&owner);
    }
    puts("Channel watch wake work: unchanged send53 + two actual waiters =55, all56 prefixes preserve publication");
}
static void watch_cross_domain(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(99);
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    size_t live=runtime_memory.live,bytes=runtime_memory.bytes;XrXirDomain *peer=NULL;
    CHECK(xr_xir_domain_new(8388608,&peer)==XR_XIR_VALUE_OK);XrXirValueAdmission admission=owner.admission;admission.domain=peer;
    XrXirChannelWatch *watch=NULL;XrXirChannelWatchState state=XR_XIR_CHANNEL_WATCH_RETRY;
    CHECK(xr_xir_channel_watch_begin(&channel,watch_sequence(&owner,&channel),&admission,&watch,&state)==XR_XIR_VALUE_OK && watch);
    xr_xir_domain_close(peer);xr_xir_domain_drop(peer);result_send(&owner,&channel,&message);
    CHECK(xr_xir_channel_watch_ready(watch));xr_xir_channel_watch_drop(watch);
    CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
#include "xir_channel_watch_controls.h"
int main(void) {
    watch_events();watch_cancel(false);watch_cancel(true);watch_owner_root();watch_changed_frontier();
    watch_failures();watch_work_prefixes();watch_cross_domain();
    watch_other_work();watch_retired_prefix();watch_thread();
    puts("Channel owned watch: actual send/receive/close notification, changed-frontier recovery, cancel under lock, external root and cross-domain lifetime physical0");return 0;
}
