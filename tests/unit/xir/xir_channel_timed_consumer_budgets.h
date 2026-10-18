/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_timed_consumer_budgets.h - Actual cumulative consumer budget cuts
 */
#ifndef XIR_CHANNEL_TIMED_CONSUMER_BUDGETS_H
#define XIR_CHANNEL_TIMED_CONSUMER_BUDGETS_H
typedef struct TimedConsumerBudgetCase {
    XrXirInstance *waiter,*peer;
    XrXirDomain *domain;
    XrXirValue channel;
    TimedSourceOutput witness;
    const uint32_t *entries;
    XrXirCallStatus expected_free;
    size_t live,bytes;
    bool send;
    unsigned capacity;
} TimedConsumerBudgetCase;
static uint64_t timed_consumer_budget_metric(XrXirDomainBudgetStats stats,unsigned axis) {
    return axis==0 ? stats.requested_bytes : axis==1 ? stats.requested_call_bytes : stats.work;
}
static uint64_t timed_consumer_budget_limit(XrXirDomainBudgetStats stats,unsigned axis) {
    return axis==0 ? stats.requested_limit : axis==1 ? stats.requested_call_limit : stats.work_limit;
}
static void timed_consumer_budget_open(XrXirProgram *program,const uint32_t entries[4],
    bool send,unsigned capacity,unsigned axis,TimedConsumerBudgetCase *owner) {
    *owner=(TimedConsumerBudgetCase){.entries=entries,.expected_free=XR_XIR_CALL_READY,
        .live=runtime_live,.bytes=runtime_bytes,.send=send,.capacity=capacity};
    XrXirInstanceConfig config={0};CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    if (config.requested_value_limit>UINT64_C(1048576)) config.requested_value_limit=UINT64_C(1048576);
    if (config.requested_call_limit>UINT64_C(1048576)) config.requested_call_limit=UINT64_C(1048576);
    if (config.work_limit>UINT64_C(1048576)) config.work_limit=UINT64_C(1048576);
    if (axis==0 && config.requested_value_limit>32768) config.requested_value_limit=32768;
    if (axis==1 && config.requested_call_limit>32768) config.requested_call_limit=32768;
    config.time=(XrXirTimeProvider){XR_XIR_CALL_ABI_VERSION,0,timed_source_clock,timed_source_offset,&owner->witness};
    XrXirInstance *maker=NULL;CHECK(xr_xir_instance_new(program,&config,&maker)==XR_XIR_CALL_READY);
    XrXirValue cap={XR_XIR_I64,0,capacity};owner->channel=timed_life_invoke(maker,entries[0],&cap,1);
    CHECK(xr_xir_instance_free(maker)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&owner->waiter)==XR_XIR_CALL_READY &&
        xr_xir_instance_new(program,&config,&owner->peer)==XR_XIR_CALL_READY);
    XrXirValue arguments[]={owner->channel,{XR_XIR_I64,0,0}};
    XrXirValue value=timed_life_invoke(owner->waiter,entries[2],arguments,2);timed_life_integer(&value,-2);
    value=timed_life_invoke(owner->peer,entries[2],arguments,2);timed_life_integer(&value,-2);
    if (!send && capacity) { value=timed_life_invoke(owner->peer,entries[1],arguments,2);timed_life_integer(&value,1123); }
    owner->domain=owner->waiter->domain;CHECK(xr_xir_instance_state(owner->waiter)==XR_XIR_INSTANCE_READY);
    CHECK(owner->waiter->budget.work_domain==owner->domain);
}
static XrXirCallStatus timed_consumer_budget_operation(TimedConsumerBudgetCase *owner) {
    XrXirValue arguments[]={owner->channel,{XR_XIR_I64,0,0}};
    XrXirCallStatus status=xr_xir_instance_start(owner->waiter,owner->entries[owner->send ? 1u : 2u],arguments,2);
    if (status==XR_XIR_CALL_READY) {
        status=timed_life_poll(owner->waiter).outcome.status;
        CHECK(status!=XR_XIR_CALL_SUSPENDED);
        owner->expected_free=status==XR_XIR_CALL_RETURNED ? XR_XIR_CALL_READY : status;
    }
    if (status==XR_XIR_CALL_RETURNED) {
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(owner->waiter,&value)==status);
        timed_life_integer(&value,owner->send ? (owner->capacity ? 1123 : -1877) : (owner->capacity ? 41 : -2));
    }
    return status;
}
static void timed_consumer_budget_close(TimedConsumerBudgetCase *owner) {
    /* A rejected replacement can exhaust shared Call storage before attaching.
     * Its terminal limit survives the retained returned Call. */
    if (owner->waiter->budget.exhausted) {
        CHECK(xr_xir_task_executor_completion_status(owner->waiter->executor)==XR_XIR_CALL_LIMIT);
        owner->expected_free=XR_XIR_CALL_LIMIT;
    }
    XrXirCallStatus released=xr_xir_instance_free(owner->waiter);
    if (released!=owner->expected_free) fprintf(stderr,"Consumer budget cleanup=%u expected=%u\n",released,owner->expected_free);
    CHECK(released==owner->expected_free && xr_xir_instance_free(owner->peer)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&owner->channel);CHECK(runtime_live==owner->live && runtime_bytes==owner->bytes);
}
static void timed_consumer_budget_spend(TimedConsumerBudgetCase *owner,unsigned axis,uint64_t required,unsigned less) {
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(owner->domain);
    uint64_t limit=timed_consumer_budget_limit(before,axis),used=timed_consumer_budget_metric(before,axis);
    CHECK(required>=less && limit-used>=required);
    uint64_t spend=limit-used-required+less;
    if (axis==2) CHECK(xr_xir_domain_work(owner->domain,spend));
    else if (spend) {
        XrXirValueStatus status=XR_XIR_VALUE_OK;
        void *memory=axis==0 ? xr_xir_domain_allocate(owner->domain,(size_t)spend,&status) :
            xr_xir_domain_call_allocate(owner->domain,spend,&status);
        CHECK(memory && status==XR_XIR_VALUE_OK);
        if (axis==0) xr_xir_domain_deallocate(owner->domain,memory,(size_t)spend);
        else xr_xir_domain_call_deallocate(owner->domain,memory,spend);
    }
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(owner->domain);
    CHECK(timed_consumer_budget_metric(after,axis)==limit-required+less);
    timed_same_consumer_ledger(&before,after);
}
static void timed_consumer_budget_repeat(TimedConsumerBudgetCase *owner,unsigned axis,uint64_t required) {
    if (required) {
        if (!owner->send && owner->capacity) {
            XrXirValue arguments[]={owner->channel,{XR_XIR_I64,0,0}};
            XrXirValue value=timed_life_invoke(owner->peer,owner->entries[1],arguments,2);
            CHECK(value.type==XR_XIR_I64 && (value.payload==1123 || value.payload==-1877));xr_xir_value_drop(&value);
        }
        CHECK(timed_consumer_budget_operation(owner)==XR_XIR_CALL_LIMIT);
    } else {
        CHECK(axis==0);XrXirValueStatus status=XR_XIR_VALUE_OK;
        CHECK(!xr_xir_domain_allocate(owner->domain,1,&status) && status==XR_XIR_VALUE_LIMIT);
    }
}
static void timed_consumer_budget_cases(XrXirProgram *program,const uint32_t entries[4]) {
    unsigned cuts=0,zero=0;
    for (unsigned send=0;send<2;++send) for (unsigned capacity=0;capacity<2;++capacity) for (unsigned axis=0;axis<3;++axis) {
        TimedConsumerBudgetCase owner;timed_consumer_budget_open(program,entries,send!=0,capacity,axis,&owner);
        XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(owner.domain);
        CHECK(timed_consumer_budget_operation(&owner)==XR_XIR_CALL_RETURNED);
        XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(owner.domain);
        uint64_t required=timed_consumer_budget_metric(after,axis)-timed_consumer_budget_metric(before,axis);
        CHECK(required || axis==0);timed_consumer_budget_close(&owner);
        for (unsigned less=0;less<(required ? 2u : 1u);++less) {
            timed_consumer_budget_open(program,entries,send!=0,capacity,axis,&owner);
            timed_consumer_budget_spend(&owner,axis,required,less);before=xr_xir_domain_budget_stats(owner.domain);
            XrXirCallStatus status=timed_consumer_budget_operation(&owner);
            if (status!=(less ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_RETURNED))
                fprintf(stderr,"Consumer budget send=%u cap=%u axis=%u required=%llu less=%u status=%u\n",
                    send,capacity,axis,(unsigned long long)required,less,status);
            CHECK(status==(less ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_RETURNED));
            timed_consumer_budget_repeat(&owner,axis,required);after=xr_xir_domain_budget_stats(owner.domain);
            CHECK(owner.waiter->domain==owner.domain && timed_consumer_budget_metric(after,axis)<=timed_consumer_budget_limit(after,axis));
            timed_same_consumer_ledger(&before,after);timed_consumer_budget_close(&owner);
            if (required) ++cuts;else ++zero;
        }
    }
    printf("Actual timed consumer budgets: %u exact/minus1 value/call/work cuts, %u zero-value-request controls; same Domain, no reset/refund/new limits and physical baseline PASS\n",cuts,zero);
}
#endif // XIR_CHANNEL_TIMED_CONSUMER_BUDGETS_H
