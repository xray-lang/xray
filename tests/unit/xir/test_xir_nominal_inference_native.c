/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_nominal_inference_native.c - Real emitted constructor execution
 *
 * KEY CONCEPT:
 *   The host compiler consumes checked C without a Source producer at runtime.
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Native nominal inference %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "nominal_inference_runtime.h"
XR_DATA const XrXirProgramSpec nominal_inference_program;
XR_DATA const uint32_t nominal_inference_ids[2];
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&nominal_inference_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);nominal_inference_run(program,nominal_inference_ids[0],nominal_inference_ids[1]);
    CHECK(!runtime_live&&!runtime_bytes&&!source_fixture_compile_live&&!source_fixture_compile_bytes);
    puts("Nominal constructor generated C and retirement PASS");return 0;
}
