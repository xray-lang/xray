/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_capture_mixed.c - Real captured Functions cross VM and native entry callbacks
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
#include "xir_generator_consumption_cases.h"
XR_DATA const XrXirProgramSpec generator_capture_program;
XR_DATA const uint32_t generator_capture_source_entry,generator_capture_function_count;
XR_DATA const uint8_t generator_capture_function_targets[];
static const XrXirCallEntry *capture_mixed_selected[64];
static bool capture_mixed_native[64];
static uint32_t capture_mixed_steps[2],capture_mixed_functions[2],capture_mixed_generators;
static XrXirAction capture_mixed_resume(XrXirCallView *view) {
    uint32_t entry=xr_xir_call_current_entry(view->activation);CHECK(entry<64 && capture_mixed_selected[entry]);
    uint32_t provider=capture_mixed_native[entry]?1u:0u;++capture_mixed_steps[provider];
    if(generator_capture_function_targets[entry])++capture_mixed_functions[provider];
    if(capture_mixed_selected[entry]->flags&XR_XIR_ENTRY_GENERATOR)++capture_mixed_generators;
    return capture_mixed_selected[entry]->resume(view);
}
int main(int argc,char **argv) {
    CHECK(argc==2);bool root_native=!strcmp(argv[1],"native-vm");CHECK(root_native || !strcmp(argv[1],"vm-native"));
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);const XrXirProgramSpec *native=&generator_capture_program;
    CHECK(native->entry_count<=64 && native->entry_count==generator_capture_function_count);
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context,native->proof.bytes,native->proof.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_lower(closed,&native->target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirVmBinding *bindings=NULL;XrXirCallEntry *vm=NULL;
    CHECK(xr_xir_compile_vm_bind_table(lowered,&bindings,&vm)==XR_XIR_OK);XrXirCallEntry entries[64];
    for(uint32_t i=0;i<native->entry_count;++i) {
        capture_mixed_native[i]=generator_capture_function_targets[i]?!root_native:root_native;
        capture_mixed_selected[i]=capture_mixed_native[i]?&native->entries[i]:&vm[i];entries[i]=*capture_mixed_selected[i];
        entries[i].resume=capture_mixed_resume;
    }
    XrXirProgramSpec spec=*native;spec.entries=entries;XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&spec,&program)==XR_XIR_OK);
    generator_consumption_case(program,generator_capture_source_entry);
    CHECK(capture_mixed_steps[0] && capture_mixed_steps[1] && capture_mixed_functions[root_native?0:1]>=4 &&
        !capture_mixed_functions[root_native?1:0] && !capture_mixed_generators);
    xr_compile_resources_free(vm);xr_compile_resources_free(bindings);xr_xir_compile_artifact_free(lowered);
    source_fixture_owner_free(&owner);CHECK(!runtime_live && !runtime_bytes);
    printf("Nested Fn/Iterator captures mixed %s: VM%u native%u, real Fn callbacks%u, cold producer not resumed, returned17/physical0 PASS\n",
        argv[1],capture_mixed_steps[0],capture_mixed_steps[1],capture_mixed_functions[0]+capture_mixed_functions[1]);return 0;
}
