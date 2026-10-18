/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_consumption_native.c - Generated C consumes real complete Iterator input
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_generator_consumption_cases.h"
XR_DATA const XrXirProgramSpec generator_consumption_program;
XR_DATA const uint32_t generator_consumption_entry;
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&generator_consumption_program,&program)==XR_XIR_OK);
    generator_consumption_case(program,generator_consumption_entry);source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);puts("Generated C complete Iterator alternative input and actual producer; physical0 PASS");return 0;
}
