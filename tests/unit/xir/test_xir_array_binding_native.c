/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Native Array binding %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "array_binding_fixture.h"
#include "array_binding_runtime.h"
XR_DATA const XrXirProgramSpec array_binding_program,array_variables_program;
XR_DATA const uint32_t array_binding_ids[6],array_variables_ids[6];
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *controls=NULL,*original=NULL;CHECK(xr_xir_compile_program_seal(&context,&array_binding_program,&controls)==XR_XIR_OK);
    CHECK(xr_xir_compile_program_seal(&context,&array_variables_program,&original)==XR_XIR_OK);xr_compile_resources_release(context.resources);
    array_binding_run(controls,array_binding_ids,false);array_binding_run(original,array_variables_ids,true);
    CHECK(!runtime_live&&!runtime_bytes&&!source_fixture_compile_live&&!source_fixture_compile_bytes);
    puts("Exact Array binding and complete original variables generated C/retirement PASS");return 0;
}
