/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * slice_u8_backend_cases.h - Shared semantic and host-admission checks
 */
#ifndef SLICE_U8_BACKEND_CASES_H
#define SLICE_U8_BACKEND_CASES_H
#include "xir/xxir_program.h"
typedef struct SliceHostCases { uint32_t read,read_function;XrXirType view; } SliceHostCases;
static inline void slice_host_rejections(XrXirProgram *program,const SliceHostCases *cases) {
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;XrXirValue function={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue forged={(uint32_t)cases->view,0,1};
    CHECK(xr_xir_instance_start(instance,cases->read,&forged,1)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start(instance,cases->read_function,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&function)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_start_function(instance,&function,&forged,1)==XR_XIR_CALL_BAD_ARGUMENT);
    xr_xir_value_drop(&function);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
static inline void slice_program_case(XrXirProgram *program,uint32_t function) {
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;XrXirValue result={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult poll=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    CHECK(poll.outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && result.payload==1);
    xr_xir_value_drop(&result);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
#endif
