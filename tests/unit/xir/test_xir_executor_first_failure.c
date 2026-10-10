/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_executor_first_failure.c - Authenticated first failure across bounded root epochs
 */
#include "first_failure_oracle_projection.h"

static void binding_rejections(XrXirCall *call) {
    const XrXirExecutorBinding actual={call->executor_owner,call->executor_activation,
        call->executor_generation,call->executor_ticket};
    XrXirCallStatus before=XR_XIR_CALL_CONSUMED;
    CHECK(xr_xir_call_driver_failure(call,&actual,&before));
    for(unsigned axis=0;axis<4;++axis) {
        XrXirExecutorBinding bad=actual;
        if(axis==0)bad.owner=&actual;
        if(axis==1)bad.activation=&bad;
        if(axis==2)++bad.generation;
        if(axis==3)++bad.ticket;
        XrXirCallStatus sentinel=XR_XIR_CALL_CONSUMED;
        CHECK(!xr_xir_call_driver_failure(call,&bad,&sentinel));
        CHECK(sentinel==XR_XIR_CALL_CONSUMED);
        XrXirCallStatus after=XR_XIR_CALL_CONSUMED;
        CHECK(xr_xir_call_driver_failure(call,&actual,&after) && after==before);
    }
    CHECK(!xr_xir_call_driver_failure(call,&actual,NULL));
}
static XrXirInstanceResult finish_root(XrXirInstance *instance) {
    XrXirInstanceResult result={0};unsigned steps=0;
    do {result=xr_xir_instance_poll_bounded(instance,1);CHECK(++steps<4096);}
    while(result.outcome.status==XR_XIR_CALL_READY);
    return result;
}
static void root_control(uint64_t work_limit,bool retry) {
    CHECK(!runtime_live && !runtime_bytes);
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirProgram *program=cleanup_program(&owner.context,PENDING_DIRECT);
    PendingOutput output={.mode=PENDING_DIRECT};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.work_limit=work_limit;
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,pending_output,&output};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    output.instance=instance;XrXirDomain *domain=instance->domain;CHECK(xr_xir_domain_retain(domain));
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    binding_rejections(instance->call);
    XrXirInstanceResult first=finish_root(instance);
    XrXirCallStatus expected=work_limit==19?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OUTPUT_ERROR;
    CHECK(first.outcome.status==expected && instance->executor->first_failure==expected);
    if(work_limit==UINT64_C(128000000) && !retry) {
        CHECK(!instance->budget.exhausted && instance->executor->shutdown_status==XR_XIR_CALL_READY);
        CHECK(!xr_xir_call_cleanup_incomplete(instance->call));
        CHECK(output.count==2 && output.values[0]==7 && output.values[1]==6);
    }
    binding_rejections(instance->call);
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(domain);
    if(retry) {
        uint64_t epoch=instance->epoch;XrXirCall *old=instance->call;
        CHECK(xr_xir_instance_start(instance,UINT32_MAX,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(instance->epoch==epoch && instance->call==old && instance->executor->first_failure==XR_XIR_CALL_OUTPUT_ERROR);
        CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
        CHECK(instance->epoch==epoch+1 && instance->executor->first_failure==XR_XIR_CALL_READY);
        XrXirInstanceResult second=finish_root(instance);
        CHECK(second.outcome.status==XR_XIR_CALL_RETURNED && instance->executor->first_failure==XR_XIR_CALL_READY);
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
        CHECK(result.type==XR_XIR_I64 && result.payload==7);xr_xir_value_drop(&result);
        XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(domain);
        CHECK(after.requested_call_bytes>before.requested_call_bytes && after.work>before.work);
    } else {
        CHECK(xr_xir_task_executor_completion_status(instance->executor)==expected);
        CHECK(xr_xir_instance_stop(instance)==expected);
    }
    XrXirCallStatus freed=xr_xir_instance_free(instance);
    CHECK(freed==(retry?XR_XIR_CALL_READY:expected));
    XrXirDomainBudgetStats fees=xr_xir_domain_budget_stats(domain);
    xr_xir_domain_drop(domain);xr_xir_compile_program_drop(program);
    CHECK(!runtime_live && !runtime_bytes);
    CHECK(library_compile_stats(&owner.context).live_bytes==owner.baseline.live_bytes);
    library_compile_owner_drop(&owner);CHECK(!source_program_compile_live && !source_program_compile_bytes);
    printf("{\"control_work\":%llu,\"retry\":%s,\"first\":%u,\"free\":%u,\"requested_call\":%llu,\"work\":%llu,\"physical_zero\":true}\n",
        (unsigned long long)work_limit,retry?"true":"false",first.outcome.status,freed,
        (unsigned long long)fees.requested_call_bytes,(unsigned long long)fees.work);
}
int main(void) {
    printf("{\"Executor\":%zu,\"Executor_align\":%zu,\"first_failure_offset\":%zu,\"Call\":%zu,\"Frame\":%zu,\"state_offset\":%zu,\"Task\":%zu}\n",
        sizeof(XrXirTaskExecutor),_Alignof(XrXirTaskExecutor),offsetof(XrXirTaskExecutor,first_failure),
        sizeof(XrXirCall),sizeof(CallFrame),(sizeof(CallFrame)+15u)&~(size_t)15u,sizeof(XirTask));
    root_control(28,false);root_control(19,false);root_control(128000000,true);
    root_control(UINT64_C(128000000),false);
    library_compile_observer_free();return 0;
}
