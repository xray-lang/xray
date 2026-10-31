/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_copy_native.c - The same copy oracle over actual generated C11.
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"native copy %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "source_copy_runtime.h"
XR_DATA const XrXirProgramSpec source_copy_program;
XR_DATA const uint32_t source_copy_selection[2];
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_FINITE,8388608,true};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&source_copy_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);XrXirValue kept=source_copy_run(program,source_copy_selection);source_copy_retired(&kept);
    puts("Generated C copy: nested COW, identity and retired String PASS");return 0;
}
