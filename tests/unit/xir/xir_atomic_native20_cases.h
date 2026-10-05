/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_native20_cases.h - Original Atomic values and runtime failure ownership
 */
#ifndef XIR_ATOMIC_NATIVE20_CASES_H
#define XIR_ATOMIC_NATIVE20_CASES_H
#include "xir/xxir_tuple.h"
enum { ATOMIC_NATIVE20_ENTRIES=18 };
static size_t atomic_runtime_sites[2];
static const int atomic_expected[20]={42,1,42,40,40,40,1,0,0,0,42,38,42,40,40,40,1,0,0,0};
static bool atomic_bool_entry(unsigned n) {return n==1 || (n>=6 && n<=9) || n>=16;}
static XrXirInstance *atomic_instance(XrXirProgram *program) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=1024*1024;config.value_limit=1024*1024;config.call_limit=1024*1024;
    config.poll_limit=1000000;config.depth_limit=64;XrXirInstance *instance=NULL;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    CHECK((status==XR_XIR_CALL_READY && instance) || (status==XR_XIR_CALL_OOM && !instance));return instance;
}
static XrXirCallResult atomic_drive(XrXirInstance *instance,uint32_t entry) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,NULL,0);
    if(status!=XR_XIR_CALL_READY)return (XrXirCallResult){.status=status};
    XrXirInstanceResult result;do{result=xr_xir_instance_poll_bounded(instance,16);}while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(!result.outcome.wake);return result.outcome;
}
static int atomic_golden(const XrXirValue *tuple,unsigned n) {
    XrXirValue value={0},flag={0};CHECK(n<18);
    CHECK(xr_xir_tuple_get(tuple,0,&value)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(tuple,1,&flag)==XR_XIR_VALUE_OK);
    CHECK(value.type==(uint32_t)(atomic_bool_entry(n)?XR_XIR_BOOL:XR_XIR_I64) && value.payload==(int64_t)atomic_expected[n]);
    bool successful=!(n==3 || n==5 || n==7 || n==9);
    CHECK(flag.type==XR_XIR_BOOL && flag.payload==(successful?1:0));
    int code=(int)value.payload;xr_xir_value_drop(&flag);xr_xir_value_drop(&value);return code;
}
static int atomic_normal(XrXirProgram *program,uint32_t entry,unsigned n) {
    XrXirInstance *instance=atomic_instance(program);CHECK(instance);
    XrXirCallResult result=atomic_drive(instance,entry);CHECK(result.status==XR_XIR_CALL_RETURNED && xr_xir_panic_empty(&result.panic));
    XrXirValue tuple={0};CHECK(xr_xir_instance_take_result(instance,&tuple)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    /* The compiled producer and execution have died before reading the escaped Tuple. */
    xr_xir_compile_program_drop(program);program=NULL;xr_xir_compile_program_drop(program);
    int code=atomic_golden(&tuple,n);xr_xir_value_drop(&tuple);
    CHECK(!tuple.type && !tuple.reserved && !tuple.payload);xr_xir_value_drop(&tuple);
    CHECK(!runtime_live && !runtime_bytes);return code;
}
static void atomic_runtime_faults(XrXirProgram *program,uint32_t entry,unsigned n) {
    CHECK(n<2);size_t compiler_live=atomic_program_compile_live,compiler_bytes=atomic_program_compile_bytes;bool complete=false;
    for(size_t ordinal=0;ordinal<127;++ordinal) {
        CHECK(!runtime_live && !runtime_bytes);runtime_attempts=0;runtime_fail_at=ordinal;
        XrXirInstance *instance=atomic_instance(program);
        XrXirCallResult result=instance?atomic_drive(instance,entry):(XrXirCallResult){.status=XR_XIR_CALL_OOM};
        size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
        if(attempts>ordinal) {
            if(result.status!=XR_XIR_CALL_OOM)fprintf(stderr,"Atomic runtime OOM case=%u ordinal=%zu attempts=%zu status=%u\n",n,ordinal,attempts,result.status);
            CHECK(result.status==XR_XIR_CALL_OOM);
            if(instance) {XrXirValue out={0};CHECK(xr_xir_instance_take_result(instance,&out)==XR_XIR_CALL_BAD_STATE);CHECK(!out.type && !out.payload && !out.reserved);}
        } else {
            CHECK(result.status==XR_XIR_CALL_RETURNED);XrXirValue tuple={0};CHECK(xr_xir_instance_take_result(instance,&tuple)==XR_XIR_CALL_RETURNED);
            CHECK(atomic_golden(&tuple,n)==atomic_expected[n]);xr_xir_value_drop(&tuple);CHECK(!tuple.type && !tuple.reserved && !tuple.payload);xr_xir_value_drop(&tuple);atomic_runtime_sites[n]=ordinal;complete=true;
        }
        if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        CHECK(!runtime_live && !runtime_bytes && atomic_program_compile_live==compiler_live && atomic_program_compile_bytes==compiler_bytes);
        if(complete)break;
    }
    CHECK(complete);
}
#endif
