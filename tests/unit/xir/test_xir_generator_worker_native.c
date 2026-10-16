/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_worker_native.c - Generated C retains exact producer worker refusal
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_generator_worker_cases.h"
XR_DATA const XrXirProgramSpec generator_worker_program;
XR_DATA const uint32_t generator_worker_ids[11];
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&generator_worker_program,&program)==XR_XIR_OK);
    worker_boundary_cases(program,generator_worker_ids);xr_xir_compile_program_drop(program);source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);puts("Generator worker native physical0/0 PASS");return 0;
}
