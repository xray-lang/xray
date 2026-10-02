/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compile_native_owner.c - Native entries checked against fixed expected values
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
extern const XrXirProgramSpec compile_owner_program;
int main(void) {
    const XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrXirCompileContext context={0}; context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&compile_owner_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && value.payload==42);
    xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_start(instance,2,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    const char *bytes=NULL; size_t length=0;
    CHECK(xr_xir_string_view(&value,&bytes,&length));
    CHECK(length==5 && !memcmp(bytes,"A\0\xe4\xb8\xad",5));
    xr_xir_value_drop(&value);
    puts("Native compiler owner: 42 and owned UTF-8 with embedded NUL");
    return 0;
}
