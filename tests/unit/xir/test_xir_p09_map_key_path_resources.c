/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_key_path_resources.c - Complete runtime faults and cancellation at typed key paths
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "p09_hash_source_check.h"
#include "p09_map_key_path_input.h"
#include "p09_hash_worker_input.h"
#include "p09_map_key_path_resource_cases.h"
int main(int argc,char **argv) {
    CHECK(argc==3);
    bool full=!strcmp(argv[1],"--full");
    CHECK(full || !strcmp(argv[1],"--normal") || !strcmp(argv[1],"--cancel"));
    key_resource_row=(uint32_t)strtoul(argv[2],NULL,10);CHECK(key_resource_row<9);
    if(key_resource_row==8)key_resource_count=3;
    else if(key_resource_row==2)key_resource_expected[0]=1;
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirSourceResult source={0};
    const char *input=key_resource_row==8?worker_source:map_key_sources[key_resource_row].source;
    CHECK(hash_source_check(&owner.context,input,&source)==XR_XIR_OK);
    XrXirArtifact *lowered=hash_source_replay(&owner.context,&source);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t entries[3]={UINT32_MAX,UINT32_MAX,UINT32_MAX},worker=UINT32_MAX;
    if(key_resource_row==8)worker_entries(module,entries,&worker);
    else for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==5 && !memcmp(module->functions[f].name,"entry",5)) {
            CHECK(entries[0]==UINT32_MAX);entries[0]=f;
        }
    CHECK(entries[0]<module->function_count);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    key_allocations(program,entries,full);
    if(full || !strcmp(argv[1],"--cancel"))key_cancellation(program,entries);
    xr_xir_compile_program_drop(program);source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Typed Map path and worker actual runtime ownership PASS");return 0;
}
