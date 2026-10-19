/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_float_native.c - Generated C11 finite floating JSON tokens
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_json_float_runtime.h"
XR_DATA const XrXirProgramSpec json_float_program;
XR_DATA const uint32_t json_float_selection[3];
int main(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_program_seal(&context,&json_float_program,&program);
    if(status!=XR_XIR_OK) {
        CHECK(!program);xr_compile_resources_release(context.resources);
        CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
        fprintf(stderr,"Floating JSON native Program refusal%u, all owners released\n",(unsigned)status);
    }
    CHECK(status==XR_XIR_OK);
    XrXirValue kept=json_float_cases(program,json_float_selection);
    xr_compile_resources_release(context.resources);json_float_retained(&kept);
    puts("Finite floating JSON native C11: all exact-bit/grammar inputs and final Nullable after every owner");return 0;
}
