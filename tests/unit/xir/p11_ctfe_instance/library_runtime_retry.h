/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * library_runtime_retry.h - Legal retries preserve actual Instance budgets
 */
static void library_native_runtime_recover(LibraryNativeOwner *owner,unsigned index,XrXirCallStatus reason) {
    XrXirInstance *instance=owner->instances[index];CHECK(instance && !instance->budget.exhausted);
    XrXirDomain *domain=instance->domain;XrXirCallBudget before_call=instance->budget;
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(domain);
    size_t saved_fail=runtime_fail_at,attempts=runtime_attempts,compiler_attempts=instance_compile_attempts;
    runtime_fail_at=SIZE_MAX;
    bool sticky=xr_xir_instance_state(instance)==XR_XIR_INSTANCE_FAILED;
    if(sticky) {
        uint64_t epoch=instance->epoch;
        for(unsigned n=0;n<2;++n) {
            CHECK(xr_xir_instance_start(instance,owner->run,NULL,0)==reason);
            XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1);
            CHECK(result.outcome.status==reason && result.epoch==epoch);
            XrXirValue occupied={XR_XIR_I64,0,54321},saved=occupied;
            CHECK(xr_xir_instance_take_result(instance,&occupied)==XR_XIR_CALL_BAD_STATE &&
                !memcmp(&occupied,&saved,sizeof(saved)));
        }
        CHECK(runtime_attempts==attempts && instance_compile_attempts==compiler_attempts);
    } else {
        CHECK(xr_xir_instance_state(instance)==XR_XIR_INSTANCE_NEW || xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY);
        CHECK(xr_xir_instance_start(instance,owner->run,NULL,0)==XR_XIR_CALL_READY);
        XrXirCallStatus status;size_t polls=0;
        do {CHECK(++polls<=65536);status=xr_xir_instance_poll_bounded(instance,1).outcome.status;} while(status==XR_XIR_CALL_READY);
        CHECK(status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==42);
        xr_xir_value_drop(&value);
    }
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(domain);
    CHECK(owner->instances[index]==instance && instance->domain==domain && instance->budget.work_domain==domain);
    CHECK(after.requested_bytes>=before.requested_bytes && after.requested_call_bytes>=before.requested_call_bytes && after.work>=before.work &&
        instance->budget.requested_bytes>=before_call.requested_bytes && instance->budget.resumes>=before_call.resumes);
    CHECK(after.requested_limit==before.requested_limit && after.requested_call_limit==before.requested_call_limit &&
        after.work_limit==before.work_limit && after.metadata_limit==before.metadata_limit && after.call_limit==before.call_limit &&
        instance->budget.byte_limit==before_call.byte_limit && instance->budget.requested_limit==before_call.requested_limit &&
        instance->budget.resume_limit==before_call.resume_limit);
    if(sticky) CHECK(after.requested_bytes==before.requested_bytes && after.requested_call_bytes==before.requested_call_bytes &&
        after.work==before.work && after.metadata_live==before.metadata_live && after.call_live==before.call_live &&
        instance->budget.requested_bytes==before_call.requested_bytes && instance->budget.resumes==before_call.resumes);
    runtime_fail_at=saved_fail;
    fprintf(stderr,"Runtime recovery same_Instance=1 same_Domain=1 same_CallBudget=1 fees_retained=1 sticky%u\n",sticky?1u:0u);
}
