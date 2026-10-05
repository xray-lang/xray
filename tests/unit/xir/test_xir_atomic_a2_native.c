/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_atomic_a2_native.c - Independent Atomic execution and ownership qualification
 *
 * KEY CONCEPT:
 *   Each backend checks fixed expectations after the ordinary source pipeline.
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_atomic_a2_execution_cases.h"
extern const XrXirProgramSpec atomic_a2_program;
extern const unsigned atomic_a2_entry;
int main(void) {
    XrCompileResourceLimits limits={UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&atomic_a2_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    atomic_execute_pair(program,atomic_a2_entry);
    puts("Atomic generated native: independent typed-output oracle and two instances passed (explicit Ordering pending)");
    return 0;
}
