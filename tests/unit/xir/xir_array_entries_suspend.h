/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_entries_suspend.h - Receiver effects precede the immutable snapshot
 */
#ifndef XIR_ARRAY_ENTRIES_SUSPEND_H
#define XIR_ARRAY_ENTRIES_SUSPEND_H
#if !defined(XR_ENTRIES_MATRIX)
static XrXirCallStatus entries_drive_suspend(XrXirInstance *instance,XrXirCallStatus status) {
    while(status==XR_XIR_CALL_READY) {
        XrXirInstanceResult next=xr_xir_instance_poll_bounded(instance,UINT64_MAX);status=next.outcome.status;
        if(status==XR_XIR_CALL_SUSPENDED) {
            CHECK(xr_xir_instance_resume(instance,next.epoch,next.outcome.wake)==XR_XIR_CALL_READY);
            status=XR_XIR_CALL_READY;
        }
    }
    return status;
}
static void entries_suspend(EntriesCompile *run) {
    uint32_t function=entries_find(run->module,"delayed");
    XrXirInstance *instance=entries_ready(run);runtime_attempts=0;
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult pause=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    CHECK(pause.outcome.status==XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_resume(instance,pause.epoch,pause.outcome.wake)==XR_XIR_CALL_READY);
    CHECK(entries_drive_suspend(instance,XR_XIR_CALL_READY)==XR_XIR_CALL_RETURNED);
    size_t sites=runtime_attempts;XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    static const int64_t expected[]={7,9};entries_numbers(&value,expected,2,true);xr_xir_value_drop(&value);
    value=entries_run(instance,entries_find(run->module,"count"));CHECK(value.payload==1);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    for(size_t failure=0;failure<sites;++failure) {
        instance=entries_ready(run);runtime_attempts=0;runtime_fail_at=failure;
        XrXirCallStatus status=xr_xir_instance_start(instance,function,NULL,0);
        status=entries_drive_suspend(instance,status);
        CHECK(status==XR_XIR_CALL_OOM&&runtime_attempts>failure);runtime_fail_at=SIZE_MAX;
        value=(XrXirValue){0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE&&!value.type&&!value.payload);
        value=entries_run(instance,entries_find(run->module,"saved"));entries_numbers(&value,expected,2,false);xr_xir_value_drop(&value);
        value=entries_run(instance,entries_find(run->module,"count"));CHECK(value.payload==0||value.payload==1);xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    }
    instance=entries_ready(run);CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
    pause=xr_xir_instance_poll_bounded(instance,UINT64_MAX);CHECK(pause.outcome.status==XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_CANCELLED);
    value=(XrXirValue){0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE&&!value.type&&!value.payload);
    value=entries_run(instance,entries_find(run->module,"saved"));entries_numbers(&value,expected,2,false);xr_xir_value_drop(&value);
    value=entries_run(instance,entries_find(run->module,"count"));CHECK(value.payload==1);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    printf("entries receiver suspend/resume/cancel once runtimeOOM%zu physical0\n",sites);
}
#endif
#endif
