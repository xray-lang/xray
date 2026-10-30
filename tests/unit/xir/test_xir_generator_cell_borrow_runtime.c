/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_cell_borrow_runtime.c - Owned captures and real consumer loans
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir_go_source_native_allocations.h"
static XrXirInstance *borrow_observed_instance;
static size_t borrow_failure_ordinals[128];
static uint32_t borrow_failure_count,borrow_failure_next;
#include "xir/xxir_task.c"
static void source_native_event_role(SourceNativeAllocationEvent *event) {
    event->injected=borrow_failure_next<borrow_failure_count && event->ordinal==borrow_failure_ordinals[borrow_failure_next];
    runtime_fail_at=event->injected?event->ordinal:SIZE_MAX;if(event->injected)++borrow_failure_next;
    XrXirInstance *instance=borrow_observed_instance;if(!instance || !instance->executor)return;
    XrXirTaskExecutor *executor=instance->executor;
    XrXirCall *call=executor->current?executor->current->call:instance->call;if(!call)return;
    event->role=executor->current && executor->current->task?2u:1u;
    if(generator_core_activation(executor,executor->current))event->role=3;
    if(call->top)event->entry=(unsigned)(call->top->entry-call->config.entries);
}
#include "xir_generator_cell_borrow_cases.h"
XR_DATA const XrXirProgramSpec generator_cell_borrow_program;
XR_DATA const uint32_t generator_cell_borrow_ids[2];
XR_DATA const XrXirProgramSpec generator_cell_weakened_program;
XR_DATA const uint32_t generator_cell_weakened_ids[2];
static const XrXirProgramSpec *borrow_input=&generator_cell_borrow_program;
static const uint32_t *borrow_ids=generator_cell_borrow_ids;
typedef struct BorrowCodeOwner {
    SourceFixtureOwner compiler;
    XrXirArtifact *lowered;
    XrXirVmBinding *bindings;
    XrXirCallEntry *vm;
    uint32_t releases;
} BorrowCodeOwner;
static bool borrow_all_native,borrow_all_vm,borrow_root_native;
static void borrow_code_release(void *opaque) {
    BorrowCodeOwner *owner=opaque;CHECK(owner && !owner->releases && owner->lowered && owner->bindings && owner->vm);
    xr_compile_resources_free(owner->vm);owner->vm=NULL;
    xr_compile_resources_free(owner->bindings);owner->bindings=NULL;
    xr_xir_compile_artifact_free(owner->lowered);owner->lowered=NULL;++owner->releases;
}
static XrXirProgram *borrow_program_new(BorrowCodeOwner *owner) {
    source_fixture_owner_new(&owner->compiler);const XrXirProgramSpec *original=borrow_input;
    CHECK(original->entry_count<=64);XrXirArtifact *checked=NULL,*closed=NULL;
    CHECK(xr_xir_compile_checked_read(&owner->compiler.context,original->proof.bytes,original->proof.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL, NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_lower(closed,&original->target, NULL,&owner->lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_vm_bind_table(owner->lowered,&owner->bindings,&owner->vm)==XR_XIR_OK);
    XrXirCallEntry entries[64];
    for(uint32_t i=0;i<original->entry_count;++i) {
        bool producer=(original->entries[i].flags&XR_XIR_ENTRY_GENERATOR)!=0;
        borrow_native[i]=borrow_all_native || (!borrow_all_vm && (producer?!borrow_root_native:borrow_root_native));
        borrow_selected[i]=borrow_native[i]?&original->entries[i]:&owner->vm[i];
        entries[i]=*borrow_selected[i];entries[i].resume=borrow_resume;
    }
    XrXirProgramSpec spec=*original;spec.entries=entries;spec.code=(XrXirCodeLease){owner,borrow_code_release};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&owner->compiler.context,&spec,&program)==XR_XIR_OK);return program;
}
static void borrow_execute(bool negative) {
    BorrowCodeOwner owner={0};XrXirProgram *program=borrow_program_new(&owner);borrow_program_owner=program;
    borrow_negative=negative;borrow_demands=borrow_producers=borrow_loans=0;CHECK(!borrow_iterator.type && !borrow_cell.type);
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,borrow_ids[negative?1:0],NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    XrXirCallStatus expected=negative?XR_XIR_CALL_BAD_ARGUMENT:XR_XIR_CALL_RETURNED;
    if(result.outcome.status!=expected || borrow_demands!=1)fprintf(stderr,"borrow negative%u status%u demand%u producer%u loan%u\n",
        negative?1u:0u,result.outcome.status,borrow_demands,borrow_producers,borrow_loans);
    CHECK(result.outcome.status==expected && !result.outcome.wake && borrow_demands==1);
    CHECK(negative?!borrow_producers && borrow_loans==1:borrow_producers && !borrow_loans);
    if(!negative) {
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && value.payload==17);xr_xir_value_drop(&value);
    } else CHECK(!result.outcome.value.type);
    CHECK(!owner.releases);xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_free(instance)==(negative?XR_XIR_CALL_BAD_ARGUMENT:XR_XIR_CALL_READY));
    CHECK(owner.releases==1 && !owner.vm && !owner.bindings && !owner.lowered);borrow_program_owner=NULL;
    borrow_retired();CHECK(xr_xir_cell_unborrowed(&borrow_cell));
    XrXirValue value={0};CHECK(xr_xir_cell_read(&borrow_cell,&value)==XR_XIR_VALUE_OK);
    CHECK(value.type==XR_XIR_I64 && value.payload==17);xr_xir_value_drop(&value);
    xr_xir_value_drop(&borrow_iterator);xr_xir_value_drop(&borrow_cell);source_fixture_owner_free(&owner.compiler);
    CHECK(!runtime_live && !runtime_bytes);
    printf("OWNED_CELL_RUNTIME borrowed%u status%u demand%u producer%u loan%u actual code death/unborrowed/physical0 PASS\n",
        negative?1u:0u,expected,borrow_demands,borrow_producers,borrow_loans);
}
#include "xir_generator_cell_borrow_faults.h"
int main(int argc,char **argv) {
    borrow_explicit_root=argc>=3 && !strcmp(argv[2],"--explicit-root");
    bool faults=argc>2 && !strcmp(argv[argc-1],"--faults");
    CHECK(argc==(2+(borrow_explicit_root?1:0)+(faults?1:0)));
    if(borrow_explicit_root){borrow_input=&generator_cell_weakened_program;borrow_ids=generator_cell_weakened_ids;}
    setvbuf(stdout,NULL,_IONBF,0);borrow_all_native=!strcmp(argv[1],"native");borrow_all_vm=!strcmp(argv[1],"vm");
    borrow_root_native=!strcmp(argv[1],"native-vm");
    CHECK(borrow_all_native || borrow_all_vm || borrow_root_native || !strcmp(argv[1],"vm-native"));
    borrow_execute(false);borrow_execute(true);if(faults)borrow_faults();
    CHECK(borrow_all_native?borrow_callbacks[1] && !borrow_callbacks[0]:borrow_all_vm?borrow_callbacks[0] && !borrow_callbacks[1]:
        borrow_callbacks[0] && borrow_callbacks[1]);
    printf("OWNED_CELL_RUNTIME %s genuine VM%u/native%u callbacks PASS\n",argv[1],borrow_callbacks[0],borrow_callbacks[1]);return 0;
}
