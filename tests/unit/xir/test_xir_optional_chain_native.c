/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_optional_chain_native.c - Execute the exact emitted field-chain program
 *
 * KEY CONCEPT:
 *   The native oracle uses the same real execution and post-retirement values.
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Native optional chain %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "optional_chain_runtime.h"
XR_DATA const XrXirProgramSpec optional_chain_program;
XR_DATA const uint32_t optional_chain_ids[2];
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&optional_chain_program,&program)==XR_XIR_OK);
    optional_run(program,optional_chain_ids);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);puts("Optional fields/generated C/retirement PASS");return 0;
}
