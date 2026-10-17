/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_timed_same_consumer_cases.h - Reuse actual bounded consumer Domains
 */
#ifndef XIR_CHANNEL_TIMED_SAME_CONSUMER_CASES_H
#define XIR_CHANNEL_TIMED_SAME_CONSUMER_CASES_H
static void timed_same_consumer_ledger(const XrXirDomainBudgetStats *before,XrXirDomainBudgetStats after) {
    CHECK(before->bound && after.bound && before->requested_limit==after.requested_limit &&
        before->requested_call_limit==after.requested_call_limit && before->work_limit==after.work_limit &&
        before->metadata_limit==after.metadata_limit && before->call_limit==after.call_limit);
    CHECK(after.requested_bytes>=before->requested_bytes && after.requested_call_bytes>=before->requested_call_bytes && after.work>=before->work);
}
static void timed_same_consumer_case(XrXirProgram *program,const uint32_t entries[4],bool send,unsigned capacity,unsigned finish) {
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
    CHECK(xr_xir_instance_new(program,&config,&waiter)==XR_XIR_CALL_READY && xr_xir_instance_new(program,&config,&peer)==XR_XIR_CALL_READY);
    XrXirValue arguments[]={channel,{XR_XIR_I64,0,0}};
    if (send && capacity) { XrXirValue initial=timed_life_invoke(peer,entries[1],arguments,2);timed_life_integer(&initial,1123); }
    arguments[1].payload=finish==3 ? 5 : XR_XIR_TIMER_MAX_MS;
    XrXirDomain *wait_domain=waiter->domain,*peer_domain=peer->domain;
    XrXirDomainBudgetStats wait_before=xr_xir_domain_budget_stats(wait_domain),peer_before=xr_xir_domain_budget_stats(peer_domain);
    uint64_t started=xr_time_monotonic_ns();CHECK(started);
    CHECK(xr_xir_instance_start(waiter,entries[send ? 1 : 2],arguments,2)==XR_XIR_CALL_READY);
    XrXirInstanceResult pending=timed_life_poll(waiter);CHECK(pending.outcome.status==XR_XIR_CALL_SUSPENDED);
    XrXirWaitRequest request={0};CHECK(xr_xir_instance_wait_request(waiter,pending.epoch,pending.outcome.wake,&request)==XR_XIR_CALL_READY && request.kind==XR_XIR_WAIT_CHANNEL);
    CHECK(xr_xir_instance_resume(waiter,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    if (finish!=3) {
        size_t attempts=runtime_attempts;
        for (unsigned i=0;i<64;++i) {
            XrXirInstanceResult next=xr_xir_instance_poll_bounded(waiter,1);
            CHECK(next.outcome.status==XR_XIR_CALL_SUSPENDED && next.epoch==pending.epoch && next.outcome.wake==pending.outcome.wake && runtime_attempts==attempts);
        }
    }
    XrXirInstanceResult completed={0};XrXirValue result={0};arguments[1].payload=0;
    if (finish==2) {
        CHECK(xr_xir_instance_cancel_current(waiter)==XR_XIR_CALL_CANCEL_REQUESTED);
        completed=timed_life_poll(waiter);CHECK(completed.outcome.status==XR_XIR_CALL_CANCELLED);
    } else {
        if (finish==3) {
            do { xr_time_sleep_ns(1000000);completed=timed_life_poll(waiter); } while(completed.outcome.status==XR_XIR_CALL_SUSPENDED);
            CHECK(xr_time_monotonic_ns()-started>=UINT64_C(5000000));
        } else {
            XrXirValue other=timed_life_invoke(peer,entries[finish ? 3u : send ? 2u : 1u],arguments,finish ? 1u : 2u);
            timed_life_integer(&other,finish ? 7 : send ? 41 : 1123);completed=timed_life_poll(waiter);
        }
        CHECK(completed.outcome.status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(waiter,&result)==XR_XIR_CALL_RETURNED);
        timed_life_integer(&result,send ? (finish==1 ? -2877 : finish==3 ? -1877 : 1123) : (finish==1 ? 44 : finish==3 ? -2 : 41));
    }
    CHECK(xr_xir_instance_resume(waiter,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    XrXirDomainBudgetStats after_primary=xr_xir_domain_budget_stats(wait_domain),peer_after=xr_xir_domain_budget_stats(peer_domain);
    timed_same_consumer_ledger(&wait_before,after_primary);timed_same_consumer_ledger(&peer_before,peer_after);
    uint64_t call_requests=waiter->budget.requested_bytes;
    if (finish!=1) {
        if (send && capacity) { XrXirValue initial=timed_life_invoke(peer,entries[2],arguments,2);timed_life_integer(&initial,41); }
        arguments[1].payload=XR_XIR_TIMER_MAX_MS;
        CHECK(xr_xir_instance_start(peer,entries[2],arguments,2)==XR_XIR_CALL_READY && timed_life_poll(peer).outcome.status==XR_XIR_CALL_SUSPENDED);
        arguments[1].payload=0;XrXirValue sent=timed_life_invoke(waiter,entries[1],arguments,2);timed_life_integer(&sent,1123);
        CHECK(timed_life_poll(peer).outcome.status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(peer,&result)==XR_XIR_CALL_RETURNED);timed_life_integer(&result,41);
    } else {
        XrXirValue closed=timed_life_invoke(waiter,entries[send ? 1u : 2u],arguments,2);
        timed_life_integer(&closed,send ? -2877 : 44);
    }
    CHECK(waiter->domain==wait_domain && peer->domain==peer_domain &&
        !memcmp(&arguments[0],&channel,sizeof(channel)) && waiter->epoch>pending.epoch);
    timed_same_consumer_ledger(&after_primary,xr_xir_domain_budget_stats(wait_domain));
    CHECK(waiter->budget.requested_bytes>=call_requests && waiter->budget.work_domain==wait_domain && peer->budget.work_domain==peer_domain);
    timed_same_consumer_ledger(&peer_after,xr_xir_domain_budget_stats(peer_domain));
    CHECK(xr_xir_instance_resume(waiter,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(waiter)==XR_XIR_CALL_READY && xr_xir_instance_free(peer)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&channel);CHECK(runtime_live==live && runtime_bytes==bytes && witness.reads);
}
static void timed_same_consumer_cases(XrXirProgram *program,const uint32_t entries[4]) {
    for (unsigned send=0;send<2;++send) for (unsigned capacity=0;capacity<2;++capacity) for (unsigned finish=0;finish<4;++finish)
        timed_same_consumer_case(program,entries,send!=0,capacity,finish);
    puts("16 actual timed Source same consumer Domains: bounded unchanged limits, cumulative requests/work, peer/Closed/cancel/Timeout reuse and physical baseline PASS");
}
#endif // XIR_CHANNEL_TIMED_SAME_CONSUMER_CASES_H
