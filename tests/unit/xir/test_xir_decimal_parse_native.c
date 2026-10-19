/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_decimal_parse_native.c - Real generated C decimal conversion ownership
 */
#include "xir/xxir_nullable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_decimal_parse_runtime.h"
XR_DATA const XrXirProgramSpec decimal_source_program;
XR_DATA const uint32_t decimal_source_functions[3];
int main(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&decimal_source_program,&program)==XR_XIR_OK);
    XrXirValue kept=decimal_program_cases(program,decimal_source_functions);xr_compile_resources_release(context.resources);decimal_retained(&kept);
    puts("Generated C11 decimal conversion: exact values, complete runtime OOM, final Nullable after every owner");return 0;
}
