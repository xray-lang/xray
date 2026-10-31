/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_typed_display_native.c - Relocated Array child through generated C
 *
 * KEY CONCEPT:
 *   The generated instruction uses its real carrier, with no type-only retag.
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"display native %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "typed_display_receiving_cases.h"
XR_DATA const XrXirProgramSpec typed_display_program;
XR_DATA const uint32_t typed_display_received_function;
int main(void) {
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&(XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY,0,true},&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&typed_display_program,&program)==XR_XIR_OK);
    uint32_t references=atomic_load_explicit(&program->references,memory_order_acquire);
    received_program(&context,program,typed_display_received_function);xr_xir_compile_program_drop(program);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    if(stats.held_bytes!=before.held_bytes)fprintf(stderr,"native compiler held before=%llu after=%llu sealed-references=%u\n",
        (unsigned long long)before.held_bytes,(unsigned long long)stats.held_bytes,references);
    CHECK(stats.held_bytes==before.held_bytes);
    xr_compile_resources_release(context.resources);puts("Generated C: real received ARRAY_GET to TO_STRING PASS");return 0;
}
