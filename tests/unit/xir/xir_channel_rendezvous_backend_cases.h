/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_rendezvous_backend_cases.h - Actual Source receivers and complete runtime failures
 */
#ifndef XIR_CHANNEL_RENDEZVOUS_BACKEND_CASES_H
#define XIR_CHANNEL_RENDEZVOUS_BACKEND_CASES_H
#include "xir/xxir_channel_rendezvous_internal.h"
static XrXirCallStatus rendezvous_backend_invoke(XrXirInstance *instance,uint32_t entry,
    const XrXirValue *arguments,uint32_t count,XrXirValue *output,XrXirCallStatus *release) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,arguments,count);
    if (status==XR_XIR_CALL_READY) {
        XrXirInstanceResult result={0};unsigned polls=0;
        do { CHECK(++polls<4096);result=xr_xir_instance_poll_bounded(instance,1); } while(result.outcome.status==XR_XIR_CALL_READY);
        status=result.outcome.status;CHECK(xr_xir_call_result_valid(&result.outcome));
        if (status==XR_XIR_CALL_RETURNED) CHECK(xr_xir_instance_take_result(instance,output)==status);
        else *release=status;
    }
    return status;
}
static size_t rendezvous_backend_once(XrXirProgram *program,const uint32_t entries[3],size_t ordinal) {
    size_t live=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;
    runtime_fail_at=ordinal==SIZE_MAX ? SIZE_MAX : begin+ordinal;
    XrXirInstanceConfig config={0};CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *maker=NULL,*receiver=NULL;XrXirDomain *host=NULL;XrXirValue channel={0},received={0},message={XR_XIR_I64,0,613};
    XrXirChannelSendWait *send=NULL;XrXirCallStatus maker_release=XR_XIR_CALL_READY,receiver_release=XR_XIR_CALL_READY;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&maker);
    if (status==XR_XIR_CALL_READY) status=rendezvous_backend_invoke(maker,entries[0],NULL,0,&channel,&maker_release);
    if (status==XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_value_valid(&channel));size_t attempts=runtime_attempts;
        CHECK(xr_xir_instance_free(maker)==XR_XIR_CALL_READY && runtime_attempts==attempts);maker=NULL;
        XrXirDomainBudgetControls controls={8388608,8388608,128000000,4194304,4194304};
        XrXirValueStatus created=xr_xir_domain_new_budgeted(8388608,&controls,&host);
        status=created==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_READY;CHECK(created==XR_XIR_VALUE_OK || created==XR_XIR_VALUE_OOM);
        if (status==XR_XIR_CALL_READY) {
            XrXirValueAdmission admission={.arena=xr_xir_value_arena(&channel),.domain=host,.work=128000000,.scratch_bytes=4194304};
            XrXirChannelSendWaitState state=XR_XIR_CHANNEL_SEND_WAIT_RETRY;
            XrXirValueStatus pending=xr_xir_channel_rendezvous_send_begin(&channel,&message,&admission,&send,&state);
            CHECK(pending==XR_XIR_VALUE_OK || pending==XR_XIR_VALUE_OOM);
            status=pending==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_READY;
            if (status==XR_XIR_CALL_READY) {
                CHECK(send && state==XR_XIR_CHANNEL_SEND_WAIT_PENDING && !((XirChannel *)object_pointer(&channel))->buffer.count);
                status=xr_xir_instance_new(program,&config,&receiver);
                if (status==XR_XIR_CALL_READY) status=rendezvous_backend_invoke(receiver,entries[1],&channel,1,&received,&receiver_release);
                if (status==XR_XIR_CALL_RETURNED) CHECK(received.type==XR_XIR_I64 && received.payload==613 &&
                    xr_xir_channel_send_wait_state(send)==XR_XIR_CHANNEL_SEND_WAIT_SENT && !((XirChannel *)object_pointer(&channel))->send_count);
            }
        }
    }
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (status!=(ordinal==SIZE_MAX ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_OOM))
        fprintf(stderr,"Rendezvous backend ordinal=%zu sites=%zu status=%u\n",ordinal,sites,status);
    CHECK(status==(ordinal==SIZE_MAX ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_OOM));
    size_t attempts=runtime_attempts;CHECK(xr_xir_instance_free(receiver)==receiver_release && xr_xir_instance_free(maker)==maker_release && runtime_attempts==attempts);
    xr_xir_value_drop(&received);xr_xir_channel_send_wait_drop(send);xr_xir_domain_drop(host);xr_xir_value_drop(&channel);
    CHECK(runtime_attempts==attempts);
    CHECK(runtime_live==live && runtime_bytes==bytes);return sites;
}
#include "xir_channel_recv_wait_backend_cases.h"
static void rendezvous_backend_execute(XrXirProgram *program,const uint32_t entries[3]) {
    CHECK(rendezvous_backend_once(program,entries,SIZE_MAX));recv_wait_backend_execute(program,entries);
    puts("Channel actual Source receiver: zero-capacity factory, creator Instance freed, independent owned sender, typed613 receive before ack and runtime physical baseline");
}
static void rendezvous_backend_faults(XrXirProgram *program,const uint32_t entries[3]) {
    size_t sites=rendezvous_backend_once(program,entries,SIZE_MAX);CHECK(sites);
    for (size_t ordinal=0;ordinal<sites;++ordinal) {
        CHECK(rendezvous_backend_once(program,entries,ordinal)>ordinal);
        CHECK(rendezvous_backend_once(program,entries,SIZE_MAX)==sites);
    }
    recv_wait_backend_faults(program,entries);
    printf("Channel actual backend zero-capacity sender/Source receiver complete runtime allocator ordinals=%zu; creator retirement and same-process recovery physical baseline\n",sites);
}
#endif // XIR_CHANNEL_RENDEZVOUS_BACKEND_CASES_H
