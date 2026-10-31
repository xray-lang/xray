/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_materialization_native.c - Execute the same Array programs through generated C
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Native Array materialization %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "array_materialization_fixture.h"
#include "array_materialization_runtime.h"
XR_DATA const XrXirProgramSpec array_materialization_program,array_demo_program;
XR_DATA const uint32_t array_materialization_ids[ARRAY_MATERIALIZATION_ENTRY_COUNT],array_demo_ids[ARRAY_MATERIALIZATION_ENTRY_COUNT];
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *controls=NULL,*original=NULL;CHECK(xr_xir_compile_program_seal(&context,&array_materialization_program,&controls)==XR_XIR_OK);
    CHECK(xr_xir_compile_program_seal(&context,&array_demo_program,&original)==XR_XIR_OK);
    array_run(controls,array_materialization_ids,false,&context);array_run(original,array_demo_ids,true,&context);
    xr_compile_resources_release(context.resources);
    CHECK(!runtime_live&&!runtime_bytes&&!source_fixture_compile_live&&!source_fixture_compile_bytes&&array_raw_view_checks==5);
    puts("Exact Array materialization and complete arrays generated C/retirement PASS");return 0;
}
