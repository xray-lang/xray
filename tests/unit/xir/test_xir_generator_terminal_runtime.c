/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_terminal_runtime.c - Real code retirement after Generator closure
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
#include "xir_generator_terminal_cases.h"
XR_DATA const XrXirProgramSpec generator_terminal_program;
XR_DATA const uint32_t generator_terminal_ids[5];
XR_DATA const XrXirProgramSpec generator_terminal_inferred_program;
XR_DATA const uint32_t generator_terminal_inferred_ids[5];
XR_DATA const XrXirProgramSpec generator_terminal_fnworker_program;
XR_DATA const uint32_t generator_terminal_fnworker_ids[5];
static const XrXirProgramSpec *terminal_input=&generator_terminal_program;
static const uint32_t *terminal_ids=generator_terminal_ids;
typedef struct TerminalCodeOwner {
    SourceFixtureOwner compiler;
    XrXirArtifact *lowered;
    XrXirVmBinding *bindings;
    XrXirCallEntry *vm;
    uint32_t releases;
} TerminalCodeOwner;
static bool terminal_all_native,terminal_all_vm,terminal_root_native;
static void terminal_code_release(void *opaque) {
    TerminalCodeOwner *owner=opaque;CHECK(owner && !owner->releases && owner->lowered && owner->bindings && owner->vm);
    xr_compile_resources_free(owner->vm);owner->vm=NULL;
    xr_compile_resources_free(owner->bindings);owner->bindings=NULL;
    xr_xir_compile_artifact_free(owner->lowered);owner->lowered=NULL;++owner->releases;
}
static XrXirProgram *terminal_program(TerminalCodeOwner *owner) {
    source_fixture_owner_new(&owner->compiler);const XrXirProgramSpec *original=terminal_input;
    CHECK(original->entry_count<=64);XrXirArtifact *checked=NULL,*closed=NULL;
    CHECK(xr_xir_compile_checked_read(&owner->compiler.context,original->proof.bytes,original->proof.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL, NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_lower(closed,&original->target, NULL,&owner->lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_vm_bind_table(owner->lowered,&owner->bindings,&owner->vm)==XR_XIR_OK);
    XrXirCallEntry entries[64];
    for(uint32_t i=0;i<original->entry_count;++i) {
        bool producer=(original->entries[i].flags&XR_XIR_ENTRY_GENERATOR)!=0;
        terminal_native[i]=terminal_all_native || (!terminal_all_vm && (producer?!terminal_root_native:terminal_root_native));
        terminal_selected[i]=terminal_native[i]?&original->entries[i]:&owner->vm[i];
        entries[i]=*terminal_selected[i];entries[i].resume=terminal_resume;
    }
    XrXirProgramSpec spec=*original;spec.entries=entries;spec.code=(XrXirCodeLease){owner,terminal_code_release};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&owner->compiler.context,&spec,&program)==XR_XIR_OK);return program;
}
static XrXirInstance *terminal_instance(XrXirProgram *program,uint32_t entry,XrXirValue *result) {
    terminal_attack=terminal_reads=terminal_mutations=0;CHECK(!terminal_iterator.type);
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,result)==XR_XIR_CALL_RETURNED);
    if(terminal_iterator.type){terminal_retired(&terminal_iterator);xr_xir_value_drop(&terminal_iterator);}
    return instance;
}
static void terminal_iterator_code_death(void) {
    TerminalCodeOwner owner={0};XrXirProgram *program=terminal_program(&owner);XrXirValue iterator={0};
    XrXirInstance *instance=terminal_instance(program,terminal_ids[3],&iterator);
    CHECK(atomic_load_explicit(&terminal_object(&iterator)->state,memory_order_acquire)==XIR_GENERATOR_PARKED);
    CHECK(!owner.releases);xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && owner.releases==1);
    CHECK(!owner.vm && !owner.bindings && !owner.lowered);terminal_retired(&iterator);
    XrCompileResourceStats retained={0};
    CHECK(xr_compile_resources_stats(owner.compiler.context.resources,&retained)==XR_COMPILE_RESOURCE_OK);
    CHECK(retained.live_bytes>owner.compiler.baseline.live_bytes);
    terminal_retired(&iterator);xr_xir_value_drop(&iterator);source_fixture_owner_free(&owner.compiler);
    CHECK(!runtime_live && !runtime_bytes);
    puts("TERMINAL_CODE: real VM code/bindings/artifact retired before external Iterator drop, physical0 PASS");
}
static void terminal_function_code_death(void) {
    TerminalCodeOwner owner={0};XrXirProgram *program=terminal_program(&owner);XrXirValue function={0},iterator={0};
    XrXirInstance *instance=terminal_instance(program,terminal_ids[4],&function);
    const XrXirFunctionBinding *binding=xr_xir_function_binding(&function);CHECK(binding && binding->capture_count==1);
    CHECK(xr_xir_value_copy(&binding->captures[0],&iterator)==XR_XIR_VALUE_OK);terminal_retired(&iterator);
    xr_xir_compile_program_drop(program);CHECK(!owner.releases);
    CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start_function(instance,&function,NULL,0)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && !owner.releases);
    CHECK(xr_xir_value_valid(&function));terminal_retired(&iterator);
    xr_xir_value_drop(&function);CHECK(owner.releases==1 && !owner.vm && !owner.bindings && !owner.lowered);
    XrCompileResourceStats retained={0};
    CHECK(xr_compile_resources_stats(owner.compiler.context.resources,&retained)==XR_COMPILE_RESOURCE_OK);
    CHECK(retained.live_bytes>owner.compiler.baseline.live_bytes);
    terminal_retired(&iterator);xr_xir_value_drop(&iterator);source_fixture_owner_free(&owner.compiler);
    CHECK(!runtime_live && !runtime_bytes);
    puts("TERMINAL_CODE: revoked Function gate holds real code until last Fn drop; Iterator survives, physical0 PASS");
}
int main(int argc,char **argv) {
    bool inferred=argc==3 && !strcmp(argv[2],"inferred");
    bool fn_worker=argc==3 && !strcmp(argv[2],"function-worker");CHECK(argc==2 || inferred || fn_worker);
    if(inferred){terminal_input=&generator_terminal_inferred_program;terminal_ids=generator_terminal_inferred_ids;}
    if(fn_worker){terminal_input=&generator_terminal_fnworker_program;terminal_ids=generator_terminal_fnworker_ids;}
    terminal_all_native=!strcmp(argv[1],"native");terminal_all_vm=!strcmp(argv[1],"vm");
    terminal_root_native=!strcmp(argv[1],"native-vm");
    CHECK(terminal_all_native || terminal_all_vm || terminal_root_native || !strcmp(argv[1],"vm-native"));
    TerminalCodeOwner owner={0};XrXirProgram *program=terminal_program(&owner);
    for(uint32_t family=0;family<3;++family)for(uint32_t attack=0;attack<(family?1u:4u);++attack)
        {terminal_require_function_worker=fn_worker && family==1;terminal_execute(program,terminal_ids[family],attack);}
    terminal_require_function_worker=false;
    xr_xir_compile_program_drop(program);CHECK(owner.releases==1);source_fixture_owner_free(&owner.compiler);
    CHECK(!runtime_live && !runtime_bytes);terminal_iterator_code_death();terminal_function_code_death();
    CHECK(terminal_all_native?terminal_steps[1] && !terminal_steps[0]:terminal_all_vm?terminal_steps[0] && !terminal_steps[1]:terminal_steps[0] && terminal_steps[1]);
    printf("TERMINAL_CAPTURE %s: genuine VM%u/native%u callbacks; 3 families, 3 worker identity attacks, 2 real last-code-lease histories, physical0 PASS\n",argv[1],terminal_steps[0],terminal_steps[1]);
    if(inferred)puts("COLD_FN_BIND real inferred Fn Generator arguments through all terminal and actual code lease histories PASS");
    if(fn_worker)puts("WORKER_FN_TERMINAL real worker demand/producer context, retired Fn captures and last code lease histories PASS");return 0;
}
