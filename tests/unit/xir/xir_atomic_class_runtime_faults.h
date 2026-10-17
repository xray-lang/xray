/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_class_runtime_faults.h - Complete runtime allocation failures and owned cleanup
 */
#ifndef XIR_ATOMIC_CLASS_RUNTIME_FAULTS_H
#define XIR_ATOMIC_CLASS_RUNTIME_FAULTS_H
static XrXirCallStatus atomic_class_fault_call(XrXirInstance *instance,uint32_t entry,XrXirValue *output) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,NULL,0);
    for (unsigned polls=0;status==XR_XIR_CALL_READY && polls<10000;++polls)
        status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
    CHECK(status!=XR_XIR_CALL_READY);
    if (status==XR_XIR_CALL_RETURNED) status=xr_xir_instance_take_result(instance,output);
    return status;
}
static XrXirCallStatus atomic_class_runtime_transaction(XrXirProgram *program,const uint32_t entries[2]) {
    XrXirInstance *instances[2]={0};XrXirValue held[2]={{0}},value={0},atomic={0};
    XrXirDomain *domain=NULL;XrXirAtomicProgress progress={0};XrXirCallStatus status=XR_XIR_CALL_READY;
    for (unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config={0};CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        status=xr_xir_instance_new(program,&config,&instances[i]);
        if (status!=XR_XIR_CALL_READY) goto done;
    }
    for (unsigned i=0;i<2;++i) {
        status=atomic_class_fault_call(instances[i],entries[0],&value);
        if (status!=XR_XIR_CALL_RETURNED) goto done;
        CHECK(value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);
        status=atomic_class_fault_call(instances[i],entries[1],&held[i]);
        if (status!=XR_XIR_CALL_RETURNED) goto done;
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);instances[i]=NULL;
    }
    XrXirValueStatus made=xr_xir_domain_new(65536,&domain);
    if (made==XR_XIR_VALUE_OOM) {status=XR_XIR_CALL_OOM;goto done;}
    CHECK(made==XR_XIR_VALUE_OK);
    for (unsigned i=0;i<2;++i) {
        XrXirValueAdmission admission={.arena=xr_xir_value_arena(&held[i]),.domain=domain,.work=10000,.scratch_bytes=65536};
        XrXirValueStatus read=xr_xir_class_get(&held[i],0,&admission,&atomic);
        if (read==XR_XIR_VALUE_OOM) {status=XR_XIR_CALL_OOM;goto done;}
        CHECK(read==XR_XIR_VALUE_OK);xr_xir_value_drop(&held[i]);
        admission.arena=xr_xir_value_arena(&atomic);
        XrXirAtomicRequest request={.receiver=&atomic,.operation=XR_XIR_ATOMIC_OPERATION_LOAD,.result_type=XR_XIR_I64};
        XrXirAtomicOutcome result=xr_xir_atomic_start(&request,&admission,NULL,&progress,&value);
        if (result.status==XR_XIR_RUN_OUT_OF_MEMORY) {status=XR_XIR_CALL_OOM;goto done;}
        CHECK(!result.continuing && result.status==XR_XIR_RUN_OK && value.type==XR_XIR_I64 && value.payload==41);
        xr_xir_atomic_progress_clear(&progress);xr_xir_value_drop(&value);xr_xir_value_drop(&atomic);
    }
    status=XR_XIR_CALL_RETURNED;
done:
    xr_xir_atomic_progress_clear(&progress);xr_xir_value_drop(&value);xr_xir_value_drop(&atomic);
    for (unsigned i=0;i<2;++i) {
        if (instances[i]) CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
        xr_xir_value_drop(&held[i]);
    }
    xr_xir_domain_drop(domain);return status;
}
static void atomic_class_runtime_faults(XrXirProgram *program,const uint32_t entries[2]) {
    size_t blocks=runtime_live,bytes=runtime_bytes,compiler_blocks=source_program_compile_live,compiler_bytes=source_program_compile_bytes;
    runtime_attempts=0;CHECK(runtime_fail_at==SIZE_MAX);
    CHECK(atomic_class_runtime_transaction(program,entries)==XR_XIR_CALL_RETURNED);
    size_t sites=runtime_attempts;CHECK(sites && runtime_live==blocks && runtime_bytes==bytes);
    for (size_t site=0;site<sites;++site) {
        runtime_attempts=0;runtime_fail_at=site;
        XrXirCallStatus status=atomic_class_runtime_transaction(program,entries);runtime_fail_at=SIZE_MAX;
        if (status!=XR_XIR_CALL_OOM) fprintf(stderr,"Atomic Class runtime OOM %zu/%zu status=%u attempts=%zu\n",site,sites,status,runtime_attempts);
        CHECK(status==XR_XIR_CALL_OOM && runtime_attempts>site && runtime_live==blocks && runtime_bytes==bytes);
        size_t consumed=runtime_attempts;
        CHECK(atomic_class_runtime_transaction(program,entries)==XR_XIR_CALL_RETURNED && runtime_attempts>consumed);
        CHECK(runtime_live==blocks && runtime_bytes==bytes);
        CHECK(source_program_compile_live==compiler_blocks && source_program_compile_bytes==compiler_bytes);
    }
    xr_xir_compile_program_drop(program);
    printf("Atomic Class runtime full OOM sites=%zu, same Program retry, physical baseline PASS\n",sites);
}
#endif // XIR_ATOMIC_CLASS_RUNTIME_FAULTS_H
