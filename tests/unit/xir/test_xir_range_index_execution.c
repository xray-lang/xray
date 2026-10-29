/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_range_index_execution.c - Real generated C scalar bounds consumer
 */
#include "base/xresource_limits.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_range_index_runtime.h"
XR_DATA const XrXirProgramSpec range_index_program;
XR_DATA const uint32_t range_index_selected[2];
int main(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&range_index_program,&program)==XR_XIR_OK);
    range_index_runtime(program,range_index_selected);xr_xir_compile_program_drop(program);
    xr_compile_resources_release(context.resources);
    puts("Generated C INDEX_CHECK: valid/430/negative length engine failure PASS");return 0;
}
