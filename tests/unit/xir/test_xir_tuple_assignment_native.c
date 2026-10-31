/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_assignment_native.c - Execute the same Tuple programs through generated C
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Native Tuple assignment %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "tuple_assignment_fixture.h"
#include "tuple_assignment_runtime.h"
XR_DATA const XrXirProgramSpec tuple_assignment_program,tuple_demo_program;
XR_DATA const uint32_t tuple_assignment_ids[2],tuple_demo_ids[2];
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *controls=NULL,*original=NULL;CHECK(xr_xir_compile_program_seal(&context,&tuple_assignment_program,&controls)==XR_XIR_OK);
    CHECK(xr_xir_compile_program_seal(&context,&tuple_demo_program,&original)==XR_XIR_OK);xr_compile_resources_release(context.resources);
    tuple_run(controls,tuple_assignment_ids,false);tuple_run(original,tuple_demo_ids,true);
    CHECK(!runtime_live&&!runtime_bytes&&!source_fixture_compile_live&&!source_fixture_compile_bytes);
    puts("Exact Tuple assignment and complete original tuples generated C/retirement PASS");return 0;
}
