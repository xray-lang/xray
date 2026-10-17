/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_class_execution.h - Shared Atomic identity outlives its Class and Program
 */
#ifndef XIR_ATOMIC_CLASS_EXECUTION_H
#define XIR_ATOMIC_CLASS_EXECUTION_H
#include "xir/xxir_class.h"
#include "xir/xxir_atomic.h"
static XrXirValue atomic_class_call(XrXirInstance *instance, uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result={0};
    for (unsigned poll=0;poll<10000;++poll) {
        result=xr_xir_instance_poll_bounded(instance,1);
        if (result.outcome.status!=XR_XIR_CALL_READY) break;
    }
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    return value;
}
static void atomic_class_execute(XrXirProgram *program, const uint32_t entries[2]) {
    XrXirInstance *instances[2]={0};XrXirValue held[2]={{0}};
    for (unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config={0};CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (unsigned i=0;i<2;++i) {
        XrXirValue value=atomic_class_call(instances[i],entries[0]);
        CHECK(value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);
        held[i]=atomic_class_call(instances[i],entries[1]);
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    }
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    for (unsigned i=0;i<2;++i) {
        XrXirValue atomic={0},value={0};
        XrXirValueAdmission admission={.arena=xr_xir_value_arena(&held[i]),.domain=domain,.work=10000,.scratch_bytes=65536};
        CHECK(xr_xir_class_get(&held[i],0,&admission,&atomic)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&held[i]);CHECK(xr_xir_value_valid(&atomic));
        admission.arena=xr_xir_value_arena(&atomic);
        XrXirAtomicRequest request={.receiver=&atomic,.operation=XR_XIR_ATOMIC_OPERATION_LOAD,.result_type=XR_XIR_I64};
        XrXirAtomicProgress progress={0};
        XrXirAtomicOutcome result=xr_xir_atomic_start(&request,&admission,NULL,&progress,&value);
        CHECK(!result.continuing && result.status==XR_XIR_RUN_OK && value.type==XR_XIR_I64 && value.payload==41);
        xr_xir_atomic_progress_clear(&progress);xr_xir_value_drop(&value);xr_xir_value_drop(&atomic);
    }
    xr_xir_domain_drop(domain);
}
#endif // XIR_ATOMIC_CLASS_EXECUTION_H
