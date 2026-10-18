/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_call_wait_controls.h - Physical failure and owner budget controls
 */
#ifndef XIR_CHANNEL_CALL_WAIT_CONTROLS_H
#define XIR_CHANNEL_CALL_WAIT_CONTROLS_H
static XrXirCallStatus channel_call_fault_operation(ResultOwner *owner,XrXirValue *channel) {
    ChannelCallWitness w={.channel=channel,.sequence=channel_call_sequence(owner,channel)};
    XrXirCallAccounting accounting={0};XrXirCallEntry entry={XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_I64,
        sizeof(uint32_t),channel_call_entry,channel_call_release,&w,0,0};
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=&accounting;config.admission=owner->admission;
    XrXirCall *call=NULL;XrXirCallStatus status=xr_xir_call_new(&config,0,NULL,0,&call);
    if (status==XR_XIR_CALL_READY) {
        status=xr_xir_call_poll_bounded(call,1).status;
        if (status==XR_XIR_CALL_SUSPENDED) {
            XrXirValue message=result_integer(73);result_send(owner,channel,&message);
            status=xr_xir_call_poll_bounded(call,8).status;
        } else if (status==XR_XIR_CALL_READY) status=xr_xir_call_poll_bounded(call,8).status;
    }
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && !accounting.live_bytes && accounting.allocations==accounting.frees);
    XirChannel *storage=(XirChannel *)object_pointer(channel);CHECK(!storage->watch_count);
    if (storage->buffer.count) {
        XrXirValue received={0};XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
        CHECK(xr_xir_channel_receive(channel,&owner->admission,&received,&state)==XR_XIR_VALUE_OK &&
            state==XR_XIR_CHANNEL_VALUE && received.payload==73);xr_xir_value_drop(&received);
    }
    return status;
}
static void channel_call_allocator_faults(void) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    size_t live=runtime_memory.live,bytes=runtime_memory.bytes,sites=0;
    for (size_t ordinal=0;ordinal<=sites;++ordinal) {
        size_t begin=runtime_memory.attempts;runtime_memory.fail_at=ordinal ? begin+ordinal-1 : SIZE_MAX;
        XrXirCallStatus status=channel_call_fault_operation(&owner,&channel);
        size_t attempts=runtime_memory.attempts-begin;runtime_memory.fail_at=SIZE_MAX;
        if (!ordinal) { CHECK(status==XR_XIR_CALL_RETURNED);sites=attempts;CHECK(sites); }
        else CHECK(status==XR_XIR_CALL_OOM && attempts>=ordinal);
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
        CHECK(channel_call_fault_operation(&owner,&channel)==XR_XIR_CALL_RETURNED);
        CHECK(runtime_memory.live==live && runtime_memory.bytes==bytes);
    }
    printf("Channel Call actual runtime allocator ordinals=%zu; same-owner recovery and exact physical baseline\n",sites);
    xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void channel_call_retired_prefix(void) {
    _Alignas(8) uint32_t prefix[2]={29,8};XrXirCall *call=NULL;size_t attempts=runtime_memory.attempts;
    CHECK(xr_xir_call_new((const XrXirCallConfig *)(const void *)prefix,0,NULL,0,&call)==XR_XIR_CALL_BAD_ABI && !call);
    prefix[0]=30;
    CHECK(xr_xir_call_new((const XrXirCallConfig *)(const void *)prefix,0,NULL,0,&call)==XR_XIR_CALL_BAD_ABI && !call);
    prefix[0]=31;
    CHECK(xr_xir_call_new((const XrXirCallConfig *)(const void *)prefix,0,NULL,0,&call)==XR_XIR_CALL_BAD_ABI && !call);
    prefix[0]=32;
    CHECK(xr_xir_call_new((const XrXirCallConfig *)(const void *)prefix,0,NULL,0,&call)==XR_XIR_CALL_BAD_ABI && !call);
    prefix[0]=XR_XIR_CALL_ABI_VERSION;
    CHECK(xr_xir_call_new((const XrXirCallConfig *)(const void *)prefix,0,NULL,0,&call)==XR_XIR_CALL_BAD_ABI && !call);
    CHECK(runtime_memory.attempts==attempts);
}
static void channel_task_discovery_limit(void) {
    ChannelTaskFixture f={0};channel_task_new(&f);XrXirValue receiver=channel_task_spawn(&f,1);
    CHECK(xr_xir_task_executor_poll(f.executor,1)==XR_XIR_CALL_SUSPENDED && f.witness.callbacks==1);
    XrXirDomainBudgetStats stats=xr_xir_domain_budget_stats(f.owner.domain);
    CHECK(xr_xir_domain_work(f.owner.domain,stats.work_limit-stats.work));
    CHECK(xr_xir_task_executor_poll(f.executor,1)==XR_XIR_CALL_RETURNED);
    CHECK(f.witness.callbacks==1 && f.witness.releases==1 && f.witness.released==XR_XIR_CALL_LIMIT);
    CHECK(!((XirChannel *)object_pointer(&f.channel))->watch_count);
    XrXirCallResult outcome={0};CHECK(xr_xir_task_copy_outcome(&receiver,&outcome)==XR_XIR_CALL_LIMIT);
    xr_xir_call_result_drop(&outcome);xr_xir_value_drop(&receiver);
    CHECK(xr_xir_task_executor_free(f.executor,NULL)==XR_XIR_CALL_LIMIT);f.executor=NULL;
    channel_task_drop(&f);
}
static void channel_call_wait_controls(void) {
    channel_call_allocator_faults();channel_call_retired_prefix();channel_task_discovery_limit();
}
#endif // XIR_CHANNEL_CALL_WAIT_CONTROLS_H
