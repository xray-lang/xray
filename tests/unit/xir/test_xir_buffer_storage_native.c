/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_buffer_storage_native.c - Independent generated-C storage consumer
 *
 * KEY CONCEPT:
 *   A genuine generated Program reaches the same native owner and retirement.
 */
#include "xir/xxir_program.h"
#include "xir/xxir_buffer_storage.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"native buffer %d: %s\n",__LINE__,#c);exit(1);}}while(0)
extern const XrXirProgramSpec buffer_storage_program;
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&buffer_storage_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED && result.type==XR_XIR_I64 && result.payload==8);
    xr_xir_value_drop(&result);CHECK(xr_xir_instance_start(instance,6,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED && result.type==XR_XIR_BUFFER_STORAGE);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    for(uint32_t f=7;f<9;++f) {
        instance=NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,f,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_BAD_STATE);
        XrXirValue rejected={0};
        CHECK(xr_xir_instance_take_result(instance,&rejected)==XR_XIR_CALL_BAD_STATE && !rejected.type && !rejected.reserved && !rejected.payload);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_BAD_STATE);
    }
    xr_xir_compile_program_drop(program);
    int64_t length=-1;CHECK(xr_xir_buffer_storage_length(&result,&length)==XR_XIR_VALUE_OK && length==13);
    xr_xir_value_drop(&result);puts("Generated C native byte storage: 8 and retired 13 PASS");return 0;
}
