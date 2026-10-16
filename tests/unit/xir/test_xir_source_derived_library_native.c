/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derived_library_native.c - Actual native cold library methods
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_derived_library_runtime.h"
XR_DATA const XrXirProgramSpec fixture_derived_library_program;
XR_DATA const uint32_t fixture_derived_library_selection[3];
int main(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&fixture_derived_library_program,&program)==XR_XIR_OK);
    XrXirValue kept=cold_runtime_results(program,fixture_derived_library_selection);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);cold_runtime_retained(&kept);
    puts("Cold derived library actual C11: private READ42, owned String after Program and compiler caller death, physical0");return 0;
}
