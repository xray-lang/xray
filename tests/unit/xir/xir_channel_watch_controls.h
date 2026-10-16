/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_watch_controls.h - Exact wake boundaries and real cross-thread notification
 */
#ifndef XIR_CHANNEL_WATCH_CONTROLS_H
#define XIR_CHANNEL_WATCH_CONTROLS_H
#include "os/os_thread.h"
static void watch_other_work(void) {
    const uint64_t required[]={3,47,4};
    for (unsigned mode=0;mode<3;++mode) for (uint64_t cut=0;cut<=required[mode];++cut) {
        ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},message=result_integer(101),output={0};
        CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
        if (mode==1) result_send(&owner,&channel,&message);
        XrXirChannelWatch *first=NULL,*second=NULL;XrXirChannelWatchState watched=XR_XIR_CHANNEL_WATCH_READY;
        uint64_t sequence=watch_sequence(&owner,&channel);
        if (mode) first=watch_new(&owner,&channel);
        if (mode==2) second=watch_new(&owner,&channel);
        owner.admission.work=cut;XrXirChannelState state=XR_XIR_CHANNEL_SENT;XrXirValueStatus status;
        if (!mode) status=xr_xir_channel_watch_begin(&channel,sequence,&owner.admission,&first,&watched);
        else if (mode==1) status=xr_xir_channel_receive_result(&channel,(XrXirType)RESULT_I64,&owner.admission,&output,&state);
        else status=xr_xir_channel_close(&channel,&owner.admission,&state);
        CHECK(status==(cut==required[mode] ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT));
        XirChannel *storage=(XirChannel *)object_pointer(&channel);
        if (cut<required[mode]) {
            CHECK(result_empty(&output) && state==XR_XIR_CHANNEL_SENT && storage->sequence==sequence &&
                !atomic_load_explicit(&storage->closed,memory_order_relaxed));
            if (!mode) CHECK(!first && watched==XR_XIR_CHANNEL_WATCH_READY && !storage->watch_count);
            else CHECK(first && !xr_xir_channel_watch_ready(first) && storage->buffer.count==(mode==1 ? 1u : 0u));
        } else if (!mode) CHECK(first && watched==XR_XIR_CHANNEL_WATCH_PENDING);
        else CHECK(xr_xir_channel_watch_ready(first) && !storage->watch_count && !storage->buffer.count);
        xr_xir_channel_watch_drop(first);xr_xir_channel_watch_drop(second);xr_xir_value_drop(&output);
        xr_xir_value_drop(&channel);result_owner_free(&owner);
    }
    puts("Channel watch registration3/receive47/close4: all57 prefixes preserve queue, sequence, watch and output");
}
static void watch_retired_prefix(void) {
    _Alignas(16) unsigned char retired[8]={0x58,0x52,0x44,0x4d,0x19,0,0,1};
    XrXirValueAdmission admission={0};admission.domain=(XrXirDomain *)(void *)retired;
    XrXirValue channel={0};XrXirChannelObservation observation={UINT64_MAX,7,5,true};
    XrXirChannelState state=XR_XIR_CHANNEL_SENT;XrXirChannelWatch *watch=NULL;XrXirChannelWatchState watched=XR_XIR_CHANNEL_WATCH_READY;
    size_t attempts=runtime_memory.attempts;
    CHECK(!xr_xir_domain_current(admission.domain));
    CHECK(xr_xir_channel_observe(&channel,&admission,&observation,&state)==XR_XIR_VALUE_BAD_ARGUMENT && observation.count==5 && state==XR_XIR_CHANNEL_SENT);
    CHECK(xr_xir_channel_watch_begin(&channel,0,&admission,&watch,&watched)==XR_XIR_VALUE_BAD_ARGUMENT &&
        !watch && watched==XR_XIR_CHANNEL_WATCH_READY && runtime_memory.attempts==attempts);
    puts("Channel watch retired25 Domain: immutable8B prefix rejection before body access or allocation");
}
typedef struct WatchThread { XrXirValue channel;XrXirValueAdmission admission; } WatchThread;
static void *watch_thread_send(void *context) {
    WatchThread *thread=context;XrXirValue value=result_integer(107);XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_send_owned(&thread->channel,&value,&thread->admission,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_SENT);
    xr_xir_value_drop(&thread->channel);return NULL;
}
static void watch_thread(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    XrXirChannelWatch *watch[64]={0};for (unsigned i=0;i<64;++i) watch[i]=watch_new(&owner,&channel);
    WatchThread context={.admission=owner.admission};CHECK(xr_xir_value_copy(&channel,&context.channel)==XR_XIR_VALUE_OK);
    size_t attempts=runtime_memory.attempts;xr_thread_t thread;CHECK(xr_thread_create(&thread,watch_thread_send,&context));
    for (unsigned i=0;i<64;++i) {
        unsigned retries=0;while (!xr_xir_channel_watch_ready(watch[i])) { CHECK(++retries<100000);xr_thread_yield(); }
    }
    CHECK(xr_thread_join(thread,NULL)==0 && runtime_memory.attempts==attempts);
    for (unsigned i=0;i<64;++i) xr_xir_channel_watch_drop(watch[i]);
    CHECK(!((XirChannel *)object_pointer(&channel))->watch_count);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
    puts("Channel watch: one actual OS sender thread wakes64 owned observers without runtime allocation; acquire-ready and physical0");
}
#endif // XIR_CHANNEL_WATCH_CONTROLS_H
