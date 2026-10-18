/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_timed_consumer_faults.h - Ready consumer allocation failure recovery
 */
#ifndef XIR_CHANNEL_TIMED_CONSUMER_FAULTS_H
#define XIR_CHANNEL_TIMED_CONSUMER_FAULTS_H
static size_t timed_consumer_fault_once(XrXirProgram *program,const uint32_t entries[4],
    bool send,unsigned capacity,bool timeout,size_t ordinal) {
    size_t live=runtime_live,bytes=runtime_bytes;TimedSourceOutput witness={0};XrXirInstanceConfig config={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    if (config.requested_value_limit>UINT64_C(1048576)) config.requested_value_limit=UINT64_C(1048576);
    if (config.requested_call_limit>UINT64_C(1048576)) config.requested_call_limit=UINT64_C(1048576);
    if (config.work_limit>UINT64_C(1048576)) config.work_limit=UINT64_C(1048576);
    config.time=(XrXirTimeProvider){XR_XIR_CALL_ABI_VERSION,0,timed_source_clock,timed_source_offset,&witness};
    XrXirInstance *maker=NULL,*waiter=NULL,*peer=NULL;
    CHECK(xr_xir_instance_new(program,&config,&maker)==XR_XIR_CALL_READY);
    XrXirValue cap={XR_XIR_I64,0,capacity},channel=timed_life_invoke(maker,entries[0],&cap,1);
    CHECK(xr_xir_instance_free(maker)==XR_XIR_CALL_READY);maker=NULL;
    CHECK(xr_xir_instance_new(program,&config,&waiter)==XR_XIR_CALL_READY &&
        xr_xir_instance_new(program,&config,&peer)==XR_XIR_CALL_READY);
    XrXirValue arguments[]={channel,{XR_XIR_I64,0,0}};
    XrXirValue warm=timed_life_invoke(waiter,entries[2],arguments,2);timed_life_integer(&warm,-2);
    warm=timed_life_invoke(peer,entries[2],arguments,2);timed_life_integer(&warm,-2);
    if (send && capacity) { warm=timed_life_invoke(peer,entries[1],arguments,2);timed_life_integer(&warm,1123); }
    XrXirDomain *wait_domain=waiter->domain,*peer_domain=peer->domain;
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(wait_domain),peer_before=xr_xir_domain_budget_stats(peer_domain);
    CHECK(xr_xir_instance_state(waiter)==XR_XIR_INSTANCE_READY && xr_xir_instance_state(peer)==XR_XIR_INSTANCE_READY);
    arguments[1].payload=timeout ? 5 : XR_XIR_TIMER_MAX_MS;
    size_t begin=runtime_attempts;runtime_fail_at=ordinal==SIZE_MAX ? SIZE_MAX : begin+ordinal;
    uint64_t started=xr_time_monotonic_ns();CHECK(started);
    XrXirCallStatus status=xr_xir_instance_start(waiter,entries[send ? 1u : 2u],arguments,2);
    XrXirInstanceResult result={0},pending={0};unsigned polls=0;
    if (status==XR_XIR_CALL_READY) {
        do {
            CHECK(++polls<4096);result=timed_life_poll(waiter);
            if (result.outcome.status==XR_XIR_CALL_SUSPENDED) {
                pending=result;
                if (!timeout) {
                    CHECK(xr_xir_instance_cancel_current(waiter)==XR_XIR_CALL_CANCEL_REQUESTED);
                    result=timed_life_poll(waiter);break;
                }
                xr_time_sleep_ns(1000000);
            }
        } while (result.outcome.status==XR_XIR_CALL_SUSPENDED);
        status=result.outcome.status;
    }
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    XrXirCallStatus expected=ordinal==SIZE_MAX ? (timeout ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_CANCELLED) : XR_XIR_CALL_OOM;
    if (status!=expected) fprintf(stderr,"Consumer send=%u capacity=%u timeout=%u ordinal=%zu sites=%zu status=%u expected=%u\n",
        send ? 1u : 0u,capacity,timeout ? 1u : 0u,ordinal,sites,status,expected);
    CHECK(status==expected && (ordinal==SIZE_MAX || sites>ordinal));
    if (status==XR_XIR_CALL_RETURNED) {
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(waiter,&value)==status);
        timed_life_integer(&value,send ? -1877 : -2);CHECK(xr_time_monotonic_ns()-started>=UINT64_C(5000000));
    }
    CHECK(xr_xir_instance_state(waiter)==XR_XIR_INSTANCE_READY && waiter->domain==wait_domain && peer->domain==peer_domain);
    XrXirDomainBudgetStats after_primary=xr_xir_domain_budget_stats(wait_domain);
    timed_same_consumer_ledger(&before,after_primary);uint64_t call_requests=waiter->budget.requested_bytes;
    arguments[1].payload=0;
    if (send && capacity) { warm=timed_life_invoke(peer,entries[2],arguments,2);timed_life_integer(&warm,41); }
    arguments[1].payload=XR_XIR_TIMER_MAX_MS;
    CHECK(xr_xir_instance_start(peer,entries[2],arguments,2)==XR_XIR_CALL_READY && timed_life_poll(peer).outcome.status==XR_XIR_CALL_SUSPENDED);
    arguments[1].payload=0;warm=timed_life_invoke(waiter,entries[1],arguments,2);timed_life_integer(&warm,1123);
    CHECK(timed_life_poll(peer).outcome.status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(peer,&warm)==XR_XIR_CALL_RETURNED);
    timed_life_integer(&warm,41);
    CHECK(waiter->domain==wait_domain && peer->domain==peer_domain && !memcmp(&arguments[0],&channel,sizeof(channel)));
    timed_same_consumer_ledger(&after_primary,xr_xir_domain_budget_stats(wait_domain));
    CHECK(waiter->budget.requested_bytes>=call_requests && waiter->budget.work_domain==wait_domain && peer->budget.work_domain==peer_domain);
    timed_same_consumer_ledger(&peer_before,xr_xir_domain_budget_stats(peer_domain));
    if (pending.epoch) CHECK(xr_xir_instance_resume(waiter,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(waiter)==XR_XIR_CALL_READY && xr_xir_instance_free(peer)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&channel);CHECK(runtime_live==live && runtime_bytes==bytes);return sites;
}
static void timed_consumer_fault_cases(XrXirProgram *program,const uint32_t entries[4],bool complete) {
    for (unsigned send=0;send<2;++send) for (unsigned capacity=0;capacity<2;++capacity) for (unsigned timeout=0;timeout<2;++timeout) {
        size_t sites=timed_consumer_fault_once(program,entries,send!=0,capacity,timeout!=0,SIZE_MAX);CHECK(sites>2 && sites<4096);
        size_t count=complete ? sites : 2;
        for (size_t ordinal=0;ordinal<count;++ordinal) timed_consumer_fault_once(program,entries,send!=0,capacity,timeout!=0,ordinal);
        printf("Ready timed consumer send=%u capacity=%u timeout=%u allocator ordinals=%zu/%zu; same Instance/Domain/Channel bounded recovery physical baseline\n",
            send,capacity,timeout,count,sites);
    }
}
#endif // XIR_CHANNEL_TIMED_CONSUMER_FAULTS_H
