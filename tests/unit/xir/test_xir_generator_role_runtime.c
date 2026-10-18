/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_role_runtime.c - Genuine consumer roles across VM and native
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
#include "xir_generator_role_cases.h"
XR_DATA const XrXirProgramSpec generator_role_program;
XR_DATA const uint32_t generator_role_ids[2];
int main(int argc,char **argv) {
    CHECK(argc==2);bool native=!strcmp(argv[1],"native"),vm_only=!strcmp(argv[1],"vm");
    bool root_native=!strcmp(argv[1],"native-vm");CHECK(native || vm_only || root_native || !strcmp(argv[1],"vm-native"));
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);const XrXirProgramSpec *original=&generator_role_program;
    CHECK(original->entry_count<=64);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,original->proof.bytes,original->proof.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_lower(closed,&original->target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirVmBinding *bindings=NULL;XrXirCallEntry *vm=NULL;
    CHECK(xr_xir_compile_vm_bind_table(lowered,&bindings,&vm)==XR_XIR_OK);XrXirCallEntry entries[64];
    for(uint32_t i=0;i<original->entry_count;++i) {
        bool producer=(original->entries[i].flags&XR_XIR_ENTRY_GENERATOR)!=0;
        role_native[i]=native || (!vm_only && (producer?!root_native:root_native));
        role_selected[i]=role_native[i]?&original->entries[i]:&vm[i];entries[i]=*role_selected[i];entries[i].resume=role_resume;
    }
    XrXirProgramSpec spec=*original;spec.entries=entries;XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&spec,&program)==XR_XIR_OK);
    role_cases(program,generator_role_ids);
    CHECK(native?role_steps[1] && !role_steps[0]:vm_only?role_steps[0] && !role_steps[1]:role_steps[0] && role_steps[1]);
    xr_xir_compile_program_drop(program);xr_compile_resources_free(vm);xr_compile_resources_free(bindings);
    xr_xir_compile_artifact_free(lowered);source_fixture_owner_free(&owner);CHECK(!runtime_live && !runtime_bytes);
    printf("CONSUMER_ROLE %s: genuine VM%u/native%u callbacks, actual role overrides stale object tag, physical0 PASS\n",argv[1],role_steps[0],role_steps[1]);return 0;
}
