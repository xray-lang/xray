/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_thread_cases.h - Concurrent producers preserve owned FIFO messages
 */
#ifndef XIR_CHANNEL_THREAD_CASES_H
#define XIR_CHANNEL_THREAD_CASES_H
#include "os/os_thread.h"
typedef struct ChannelThread {
    XrXirValue channel;
    XrXirValueAdmission admission;
    unsigned producer;
    size_t sent, received;
} ChannelThread;
static void *queue_producer(void *context) {
    ChannelThread *thread=context;
    for (int64_t number=0;number<1024;++number) {
        XrXirValue value=integer((int64_t)thread->producer*10000+number);
        bool sent=false;
        for (uint32_t retry=0;retry<100000 && !sent;++retry) {
            XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
            CHECK(xr_xir_channel_send_owned(&thread->channel,&value,&thread->admission,&state)==XR_XIR_VALUE_OK);
            sent=state==XR_XIR_CHANNEL_SENT;
            if (!sent) {
                CHECK((state==XR_XIR_CHANNEL_RETRY || state==XR_XIR_CHANNEL_FULL) && value.type==XR_XIR_I64);
                xr_thread_yield();
            }
        }
        CHECK(sent && empty(&value)); ++thread->sent;
    }
    xr_xir_value_drop(&thread->channel); return NULL;
}
static void *queue_consumer(void *context) {
    ChannelThread *thread=context; int64_t next[2]={0,0};
    for (size_t count=0;count<2048;++count) {
        XrXirValue value={0}; bool received=false;
        for (uint32_t retry=0;retry<100000 && !received;++retry) {
            XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
            CHECK(xr_xir_channel_receive(&thread->channel,&thread->admission,&value,&state)==XR_XIR_VALUE_OK);
            received=state==XR_XIR_CHANNEL_VALUE;
            if (!received) {
                CHECK((state==XR_XIR_CHANNEL_RETRY || state==XR_XIR_CHANNEL_EMPTY) && empty(&value));
                xr_thread_yield();
            }
        }
        CHECK(received && value.type==XR_XIR_I64);
        int64_t producer=value.payload/10000, number=value.payload%10000;
        CHECK(producer>=0 && producer<2 && number==next[producer] && number<1024);
        ++next[producer]; ++thread->received; xr_xir_value_drop(&value);
    }
    CHECK(next[0]==1024 && next[1]==1024);
    xr_xir_value_drop(&thread->channel); return NULL;
}
static void thread_cases(void) {
    ChannelOwner owner={0}; owner_ready(&owner); XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)CH_I64,7,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    ChannelThread threads[3]={0}; xr_thread_t native[3];
    for (unsigned i=0;i<3;++i) {
        threads[i].admission=owner.admission; threads[i].producer=i;
        CHECK(xr_xir_value_copy(&channel,&threads[i].channel)==XR_XIR_VALUE_OK);
    }
    CHECK(xr_thread_create(&native[2],queue_consumer,&threads[2]));
    CHECK(xr_thread_create(&native[0],queue_producer,&threads[0]));
    CHECK(xr_thread_create(&native[1],queue_producer,&threads[1]));
    for (unsigned i=0;i<3;++i) CHECK(xr_thread_join(native[i],NULL)==0);
    CHECK(threads[0].sent==1024 && threads[1].sent==1024 && threads[2].received==2048);
    receive_number(&owner,&channel,0,XR_XIR_CHANNEL_EMPTY);
    xr_xir_value_drop(&channel); owner_free(&owner);
    puts("Channel real threads: producers=2 messages=2048 per-producer FIFO physical=0");
}
#endif
