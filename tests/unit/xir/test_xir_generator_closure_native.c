/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_closure_native.c - Generated C consumes real Generator yields
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_generator_wait_cases.h"
#include "xir_generator_cell_escape_cases.h"
XR_DATA const XrXirProgramSpec generator_closure_program;
XR_DATA const uint32_t generator_closure_ids[13];
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&generator_closure_program,&program)==XR_XIR_OK);
    uint32_t ids[13];for(unsigned n=0;n<13;++n) ids[n]=generator_closure_ids[n];
    uint32_t factories[]={ids[0],ids[8],ids[9],ids[10],ids[11],ids[12]};
    for(unsigned n=0;n<6;++n) {CHECK(xr_xir_compile_program_retain(program));ids[0]=factories[n];generator_wait_cases(program,ids);}
    closure_cell_escape_case(program,ids);source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);puts("Generated C Iterator pull caching and exact transfer; physical0 PASS");return 0;
}
