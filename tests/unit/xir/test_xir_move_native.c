/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_move_native.c - Real generated-C consuming calls
 *
 * KEY CONCEPT:
 *   The compiled body comes from the same cold declaration and ownership CFG.
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"move native %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "move_runtime.h"
XR_DATA const XrXirProgramSpec move_values_program;
XR_DATA const uint32_t move_values_entry;
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&move_values_program,&program)==XR_XIR_OK);
    move_test_run(program,move_values_entry,&context);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    xr_compile_resources_release(context.resources);CHECK(!runtime_live&&!runtime_bytes);return 0;
}
