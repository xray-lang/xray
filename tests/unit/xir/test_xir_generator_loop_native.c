/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_loop_native.c - Generated C consumes real Generator yields
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_generator_wait_cases.h"
#include "xir_generator_loop_cases.h"
XR_DATA const XrXirProgramSpec generator_loop_program;
XR_DATA const uint32_t generator_loop_ids[23];
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&generator_loop_program,&program)==XR_XIR_OK);
    CHECK(xr_xir_compile_program_retain(program));generator_wait_cases(program,generator_loop_ids);
    generator_loop_cases(program,generator_loop_ids);xr_xir_compile_program_drop(program);source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);puts("Generated C Iterator pull caching and exact transfer; physical0 PASS");return 0;
}
