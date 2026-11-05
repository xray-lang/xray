/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * string_bytes_cases.h - Fixed semantic results across backend selections
 */
#ifndef STRING_BYTES_CASES_H
#define STRING_BYTES_CASES_H
#include "xir/xxir_program.h"
#include <string.h>
typedef struct BytesHostCases { uint32_t read,loan,read_function,loan_function;XrXirType view; } BytesHostCases;
static inline void bytes_host_rejections(XrXirProgram *program,const BytesHostCases *cases) {
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue forged={(uint32_t)cases->view,0,1},ordinary={XR_XIR_I64,0,0},function={0};
    CHECK(xr_xir_instance_start(instance,cases->read,&forged,1)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start(instance,cases->loan,&ordinary,1)==XR_XIR_CALL_BAD_ARGUMENT);
    uint32_t factories[2]={cases->read_function,cases->loan_function};
    for (uint32_t f=0;f<2;++f) {
        CHECK(xr_xir_instance_start(instance,factories[f],NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&function)==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_start_function(instance,&function,f?&ordinary:&forged,1)==XR_XIR_CALL_BAD_ARGUMENT);
        xr_xir_value_drop(&function);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
static inline void bytes_program_cases(XrXirProgram *program,uint32_t function) {
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;XrXirValue result={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult poll=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    if (poll.outcome.status!=XR_XIR_CALL_RETURNED)
        fprintf(stderr,"bytes run status=%u type=%u value=%lld\n",poll.outcome.status,
            poll.outcome.value.type,(long long)poll.outcome.value.payload);
    CHECK(poll.outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(result.type==XR_XIR_I64 && result.payload==342);
    xr_xir_value_drop(&result);
}
#endif // STRING_BYTES_CASES_H
