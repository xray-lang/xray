/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_blocking_lifecycle_cases.h - Actual Source waits after creator retirement
 */
#ifndef XIR_CHANNEL_BLOCKING_LIFECYCLE_CASES_H
#define XIR_CHANNEL_BLOCKING_LIFECYCLE_CASES_H
static XrXirInstanceResult blocking_lifecycle_poll(XrXirInstance *instance) {
    unsigned polls=0;XrXirInstanceResult result={0};
    do { CHECK(++polls<4096);result=xr_xir_instance_poll_bounded(instance,1); } while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(xr_xir_call_result_valid(&result.outcome));return result;
}
static XrXirValue blocking_lifecycle_invoke(XrXirInstance *instance,uint32_t entry,const XrXirValue *argument) {
    CHECK(xr_xir_instance_start(instance,entry,argument,argument ? 1u : 0u)==XR_XIR_CALL_READY);
    CHECK(blocking_lifecycle_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void blocking_lifecycle_case(XrXirProgram *program,const uint32_t entries[5],unsigned operation,unsigned finish) {
    size_t live=runtime_live,bytes=runtime_bytes;XrXirInstanceConfig config={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *maker=NULL,*waiter=NULL,*peer=NULL;
    CHECK(xr_xir_instance_new(program,&config,&maker)==XR_XIR_CALL_READY);
    XrXirValue channel=blocking_lifecycle_invoke(maker,entries[0],NULL);
    CHECK(xr_xir_instance_free(maker)==XR_XIR_CALL_READY);maker=NULL;
    CHECK(xr_xir_instance_new(program,&config,&waiter)==XR_XIR_CALL_READY &&
        xr_xir_instance_new(program,&config,&peer)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(waiter,entries[operation+1],&channel,1)==XR_XIR_CALL_READY);
    XrXirInstanceResult pending=blocking_lifecycle_poll(waiter);CHECK(pending.outcome.status==XR_XIR_CALL_SUSPENDED);
    XrXirWaitRequest request={0};CHECK(xr_xir_instance_wait_request(waiter,pending.epoch,pending.outcome.wake,&request)==XR_XIR_CALL_READY && request.kind==XR_XIR_WAIT_CHANNEL);
    CHECK(xr_xir_instance_resume(waiter,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    size_t attempts=runtime_attempts;
    for (unsigned i=0;i<64;++i) {
        XrXirInstanceResult next=xr_xir_instance_poll_bounded(waiter,1);
        CHECK(next.outcome.status==XR_XIR_CALL_SUSPENDED && next.epoch==pending.epoch && next.outcome.wake==pending.outcome.wake);
        CHECK(runtime_attempts==attempts);
    }
    XrXirValue result={0};XrXirInstanceResult completed={0};
    if (finish==2) {
        CHECK(xr_xir_instance_cancel_current(waiter)==XR_XIR_CALL_CANCEL_REQUESTED);
        completed=blocking_lifecycle_poll(waiter);CHECK(completed.outcome.status==XR_XIR_CALL_CANCELLED);
    } else {
        XrXirValue peer_result=blocking_lifecycle_invoke(peer,entries[finish ? 4u : operation ? 1u : 2u],&channel);
        CHECK(peer_result.type==XR_XIR_I64 && peer_result.payload==(finish ? 7 : operation ? 1 : 41));xr_xir_value_drop(&peer_result);
        completed=blocking_lifecycle_poll(waiter);
        CHECK(completed.outcome.status==(finish && !operation ? XR_XIR_CALL_THROWN : XR_XIR_CALL_RETURNED));
        CHECK(xr_xir_instance_take_result(waiter,&result)==completed.outcome.status);
        if (finish && !operation) {
            XrXirEnumBorrow error={0};CHECK(xr_xir_enum_borrow(&result,&error)==XR_XIR_VALUE_OK &&
                error.name.length==10 && !memcmp(error.name.bytes,"SendResult",10) &&
                error.member.length==6 && !memcmp(error.member.bytes,"Closed",6) && !error.field_count);
        } else CHECK(result.type==XR_XIR_I64 && result.payload==(operation==1 ? (finish ? 44 : 41) : 1));
    }
    CHECK(xr_xir_instance_resume(waiter,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    xr_xir_value_drop(&result);CHECK(xr_xir_instance_free(waiter)==XR_XIR_CALL_READY && xr_xir_instance_free(peer)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&channel);CHECK(runtime_live==live && runtime_bytes==bytes);
}
static void blocking_lifecycle_cases(XrXirProgram *program,const uint32_t entries[5]) {
    for (unsigned operation=0;operation<3;++operation) for (unsigned finish=0;finish<3;++finish)
        blocking_lifecycle_case(program,entries,operation,finish);
    puts("Blocking actual Source: creator retired, send/Recv/bare Unit each64 pending polls, arguments/default once, actual peer/Closed/cancel, stale host wake denied and physical baseline");
}
#endif // XIR_CHANNEL_BLOCKING_LIFECYCLE_CASES_H
