/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_capture_native.c - Generated C authenticates complete nested captures
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_generator_consumption_cases.h"
XR_DATA const XrXirProgramSpec generator_capture_program;
XR_DATA const uint32_t generator_capture_source_entry;
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&generator_capture_program,&program)==XR_XIR_OK);
    generator_consumption_case(program,generator_capture_source_entry);source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Nested Fn/Iterator physical captures: genuine generated C returned17, physical0 PASS");return 0;
}
