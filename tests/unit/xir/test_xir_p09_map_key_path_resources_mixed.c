/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_key_path_resources_mixed.c - Faults across authenticated calls in both code directions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "p09_hash_source_check.h"
#include "p09_map_key_path_mixed_input.h"
#include "p09_map_key_path_resource_cases.h"
#include "p09_hash_foreach_mixed_code.h"
extern const XrXirProgramSpec *const p09_key_bridge_programs[9];
extern const uint32_t p09_key_bridge_entries[9][3],p09_key_bridge_targets[9][3];
static XrXirProgram *key_mixed_program(const XrXirCompileContext *context,ForeachMixedCode *code,
    ForeachMixedLease *lease,const XrXirProgramSpec *native,const uint32_t *selected,unsigned count) {
    CHECK(count && count<=3 && code->artifact && code->owners && code->owners<UINT32_MAX && !lease->code);
    CHECK(xr_xir_compile_vm_bind_table(code->artifact,&lease->bindings,&lease->entries)==XR_XIR_OK);
    for(unsigned i=0;i<count;++i) {
        CHECK(selected[i]<native->entry_count &&
            lease->entries[selected[i]].resume!=native->entries[selected[i]].resume);
        lease->entries[selected[i]]=native->entries[selected[i]];
    }
    lease->code=code;++code->owners;
    XrXirProgramSpec spec=*native;spec.entries=lease->entries;
    spec.code=(XrXirCodeLease){lease,foreach_code_release};
    XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_program_seal(context,&spec,&program);
    if(status!=XR_XIR_OK)foreach_code_release(lease);
    CHECK(status==XR_XIR_OK && program);return program;
}
int main(int argc,char **argv) {
    CHECK(argc==3);bool full=!strcmp(argv[1],"--full");
    CHECK(full || !strcmp(argv[1],"--normal") || !strcmp(argv[1],"--cancel"));
    key_resource_row=(uint32_t)strtoul(argv[2],NULL,10);CHECK(key_resource_row<9);
    if(key_resource_row==8)key_resource_count=3;
    else if(key_resource_row==2)key_resource_expected[0]=1;
    char text[8192];key_mixed_source(key_resource_row,text,sizeof(text));
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirSourceResult source={0};CHECK(hash_source_check(&owner.context,text,&source)==XR_XIR_OK);
    ForeachMixedCode code={hash_source_replay(&owner.context,&source),1};
    uint32_t entries[3]={0},targets[3]={0};
    key_mixed_entries(xr_xir_compile_artifact_module(code.artifact),key_resource_row,entries,targets);
    CHECK(!memcmp(entries,p09_key_bridge_entries[key_resource_row],sizeof(entries)) &&
        !memcmp(targets,p09_key_bridge_targets[key_resource_row],sizeof(targets)));
    for(unsigned direction=0;direction<2;++direction) {
        ForeachMixedLease lease={0};
        XrXirProgram *program=key_mixed_program(&owner.context,&code,&lease,p09_key_bridge_programs[key_resource_row],
            direction?entries:targets,key_resource_count);
        key_allocations(program,entries,full);
        if(full || !strcmp(argv[1],"--cancel"))key_cancellation(program,entries);
        xr_xir_compile_program_drop(program);
        CHECK(!runtime_live && !runtime_bytes);
        CHECK(lease.releases==1 && !lease.code && !lease.bindings && !lease.entries && code.owners==1);
    }
    foreach_code_drop(&code);CHECK(!code.artifact && !code.owners);
    source_fixture_owner_free(&owner);
    puts("Both actual mixed call directions, typed Map paths and Task containers, physical refund PASS");return 0;
}
