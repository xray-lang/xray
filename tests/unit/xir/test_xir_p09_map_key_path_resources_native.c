/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_key_path_resources_native.c - Real generated C faults and cancellation
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "p09_map_key_path_input.h"
#include "p09_map_key_path_resource_cases.h"
extern const XrXirProgramSpec *const p09_map_key_path_programs[8];
extern const uint32_t p09_map_key_path_ids[8];
extern const XrXirProgramSpec p09_hash_worker_program;
extern const uint32_t p09_hash_worker_ids[3];
int main(int argc,char **argv) {
    CHECK(argc==3);bool full=!strcmp(argv[1],"--full");
    CHECK(full || !strcmp(argv[1],"--normal") || !strcmp(argv[1],"--cancel"));
    key_resource_row=(uint32_t)strtoul(argv[2],NULL,10);CHECK(key_resource_row<9);
    if(key_resource_row==8)key_resource_count=3;
    else if(key_resource_row==2)key_resource_expected[0]=1;
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    const XrXirProgramSpec *spec=key_resource_row==8?&p09_hash_worker_program:p09_map_key_path_programs[key_resource_row];
    const uint32_t *entries=key_resource_row==8?p09_hash_worker_ids:p09_map_key_path_ids+key_resource_row;
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,spec,&program)==XR_XIR_OK);
    key_allocations(program,entries,full);
    if(full || !strcmp(argv[1],"--cancel"))key_cancellation(program,entries);
    xr_xir_compile_program_drop(program);source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Actual C11 typed Map path and worker runtime ownership PASS");return 0;
}
