/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_timed_lifecycle_cases.h - Timed Source waits after creator retirement
 */
#ifndef XIR_CHANNEL_TIMED_LIFECYCLE_CASES_H
#define XIR_CHANNEL_TIMED_LIFECYCLE_CASES_H
static XrXirInstanceResult timed_life_poll(XrXirInstance *instance) {
    unsigned polls=0;XrXirInstanceResult result={0};
    do { CHECK(++polls<4096);result=xr_xir_instance_poll_bounded(instance,1); } while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_result_valid(&result.outcome));return result;
}
static XrXirValue timed_life_invoke(XrXirInstance *instance,uint32_t entry,const XrXirValue *arguments,uint32_t count) {
    CHECK(xr_xir_instance_start(instance,entry,arguments,count)==XR_XIR_CALL_READY);
    CHECK(timed_life_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void timed_life_integer(XrXirValue *value,int64_t expected) {
    CHECK(value->type==XR_XIR_I64 && (int64_t)value->payload==expected);xr_xir_value_drop(value);
}
static void timed_life_case(XrXirProgram *program,const uint32_t entries[4],bool send,unsigned capacity,unsigned finish) {
    size_t live=runtime_live,bytes=runtime_bytes;TimedSourceOutput witness={0};XrXirInstanceConfig config={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.time=(XrXirTimeProvider){XR_XIR_CALL_ABI_VERSION,0,timed_source_clock,timed_source_offset,&witness};
    XrXirInstance *maker=NULL,*waiter=NULL,*peer=NULL;
    CHECK(xr_xir_instance_new(program,&config,&maker)==XR_XIR_CALL_READY);
    XrXirValue cap={XR_XIR_I64,0,capacity},channel=timed_life_invoke(maker,entries[0],&cap,1);
    CHECK(xr_xir_instance_free(maker)==XR_XIR_CALL_READY);maker=NULL;
    CHECK(xr_xir_instance_new(program,&config,&waiter)==XR_XIR_CALL_READY && xr_xir_instance_new(program,&config,&peer)==XR_XIR_CALL_READY);
    XrXirValue arguments[]={channel,{XR_XIR_I64,0,0}};
    if (send && capacity) { XrXirValue initial=timed_life_invoke(peer,entries[1],arguments,2);timed_life_integer(&initial,1123); }
    arguments[1].payload=finish==3 ? 5 : XR_XIR_TIMER_MAX_MS;
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
    CHECK(xr_xir_instance_free(waiter)==XR_XIR_CALL_READY && xr_xir_instance_free(peer)==XR_XIR_CALL_READY);waiter=NULL;peer=NULL;
    if (finish>=2) {
        CHECK(xr_xir_instance_new(program,&config,&waiter)==XR_XIR_CALL_READY && xr_xir_instance_new(program,&config,&peer)==XR_XIR_CALL_READY);
        if (send && capacity) { XrXirValue initial=timed_life_invoke(peer,entries[2],arguments,2);timed_life_integer(&initial,41); }
        arguments[1].payload=XR_XIR_TIMER_MAX_MS;
        CHECK(xr_xir_instance_start(peer,entries[2],arguments,2)==XR_XIR_CALL_READY && timed_life_poll(peer).outcome.status==XR_XIR_CALL_SUSPENDED);
        arguments[1].payload=0;XrXirValue sent=timed_life_invoke(waiter,entries[1],arguments,2);timed_life_integer(&sent,1123);
        CHECK(timed_life_poll(peer).outcome.status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(peer,&result)==XR_XIR_CALL_RETURNED);timed_life_integer(&result,41);
        CHECK(xr_xir_instance_free(waiter)==XR_XIR_CALL_READY && xr_xir_instance_free(peer)==XR_XIR_CALL_READY);
    }
    xr_xir_value_drop(&channel);CHECK(runtime_live==live && runtime_bytes==bytes && witness.reads);
}
static void timed_life_cases(XrXirProgram *program,const uint32_t entries[4]) {
    for (unsigned send=0;send<2;++send) for (unsigned capacity=0;capacity<2;++capacity) for (unsigned finish=0;finish<4;++finish)
        timed_life_case(program,entries,send!=0,capacity,finish);
    puts("16 timed Source lifecycle controls: creator retired, receiver/message/duration order123 once,64 stable pending polls, real peer/Closed/cancel/Timeout, stale host wake refused, same Channel/Domain recovery physical baseline");
}
#endif // XIR_CHANNEL_TIMED_LIFECYCLE_CASES_H
