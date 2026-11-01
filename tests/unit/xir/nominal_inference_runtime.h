/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * nominal_inference_runtime.h - Independent concrete constructor result oracle
 *
 * KEY CONCEPT:
 *   VM and generated C execute the same fully received instance after retirement.
 */
#ifndef NOMINAL_INFERENCE_RUNTIME_H
#define NOMINAL_INFERENCE_RUNTIME_H
#include "xir/xxir_output.h"
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
static void nominal_inference_run(XrXirProgram *program,uint32_t entry,uint32_t answer) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    for(unsigned generation=0;generation<2;++generation) {
        XrXirInstance *instance=NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_start(instance,answer,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64&&value.payload==42);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&value);
    }
    xr_xir_compile_program_drop(program);
}
#endif // NOMINAL_INFERENCE_RUNTIME_H
