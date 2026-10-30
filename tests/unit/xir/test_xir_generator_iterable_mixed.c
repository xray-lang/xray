/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_iterable_mixed.c - Actual producer and consumer callbacks cross providers
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
#include "xir_generator_iterable_cases.h"
XR_DATA const XrXirProgramSpec generator_iterable_program;
XR_DATA const uint32_t generator_iterable_ids[10];
static const XrXirCallEntry *mixed_selected[128];
static unsigned mixed_steps[2],mixed_yields[2],mixed_demands[2],mixed_timers[2];
static bool mixed_native[128];
static XrXirAction mixed_resume(XrXirCallView *view) {
    uint32_t entry=xr_xir_call_current_entry(view->activation);CHECK(entry<128 && mixed_selected[entry]);
    unsigned provider=mixed_native[entry] ? 1u : 0u;++mixed_steps[provider];
    XrXirAction action=mixed_selected[entry]->resume(view);
    if(action.kind==XR_XIR_ACTION_GENERATOR_YIELD) ++mixed_yields[provider];
    if(action.kind==XR_XIR_ACTION_AWAIT_ITERATOR) ++mixed_demands[provider];
    if(action.kind==XR_XIR_ACTION_TIMER) ++mixed_timers[provider];
    return action;
}
int main(int argc,char **argv) {
    CHECK(argc==2);bool root_native=!strcmp(argv[1],"native-vm");CHECK(root_native || !strcmp(argv[1],"vm-native"));
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    const XrXirProgramSpec *native=&generator_iterable_program;
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(native->entry_count<=128);
    CHECK(xr_xir_compile_checked_read(&owner.context,native->proof.bytes,native->proof.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL, NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_lower(closed,&native->target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirVmBinding *bindings=NULL;XrXirCallEntry *vm=NULL;CHECK(xr_xir_compile_vm_bind_table(lowered,&bindings,&vm)==XR_XIR_OK);
    XrXirCallEntry entries[128];
    for(uint32_t i=0;i<native->entry_count;++i) {
        bool producer=(native->entries[i].flags & XR_XIR_ENTRY_GENERATOR)!=0;
        mixed_native[i]=producer ? !root_native : root_native;
        mixed_selected[i]=mixed_native[i] ? &native->entries[i] : &vm[i];entries[i]=*mixed_selected[i];
        entries[i].resume=mixed_resume;
    }
    XrXirProgramSpec spec=*native;spec.entries=entries;XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&spec,&program)==XR_XIR_OK);
    generator_iterable_cases(program,generator_iterable_ids);xr_xir_compile_program_drop(program);
    CHECK(mixed_steps[0] && mixed_steps[1] && mixed_yields[root_native ? 0 : 1]==11 &&
        !mixed_yields[root_native ? 1 : 0] && mixed_timers[root_native ? 1 : 0]==12 && !mixed_timers[root_native ? 0 : 1]);
    xr_compile_resources_free(vm);xr_compile_resources_free(bindings);xr_xir_compile_artifact_free(lowered);
    source_fixture_owner_free(&owner);CHECK(!runtime_live && !runtime_bytes);
    printf("Iterator wait mixed %s: VM%u native%u yields%u rootDemands%u producerDemands%u timers%u physical0 PASS\n",
        argv[1],mixed_steps[0],mixed_steps[1],mixed_yields[0]+mixed_yields[1],mixed_demands[root_native ? 1 : 0],
        mixed_demands[root_native ? 0 : 1],mixed_timers[0]+mixed_timers[1]);return 0;
}
