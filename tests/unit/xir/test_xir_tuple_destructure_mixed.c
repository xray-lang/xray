/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_destructure_mixed.c - Owned grouped bindings across backend calls
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "tuple_destructure_cases.h"
XR_DATA const XrXirProgramSpec tuple_destructure_program;
XR_DATA const uint32_t tuple_destructure_make;
typedef struct DestructureMixedOwner {
    XrXirProgram *vm_program;
    XrXirCallEntry *entries;
} DestructureMixedOwner;
static unsigned releases;
static void destructure_mixed_free(void *pointer) {
    DestructureMixedOwner *owner=pointer;
    xr_compile_resources_free(owner->entries);
    xr_xir_compile_program_drop(owner->vm_program);
    xr_compile_resources_free(owner);
    ++releases;
}
static XrXirProgram *destructure_mixed_build(unsigned mode) {
    CHECK(mode<4);
    const XrXirProgramSpec *native=&tuple_destructure_program;
    const XrXirCompileContext *context=effects_source_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrXirArtifact *checked=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,native->proof.bytes,native->proof.length,&checked,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    CHECK(module && module->function_count==native->entry_count);
    CHECK(tuple_destructure_make<module->function_count);
    const XrXirFunction *make=&module->functions[tuple_destructure_make];
    CHECK(make->name_length==4 && !memcmp(make->name,"make",4) && !make->parameter_count);
    DestructureMixedOwner *owner=NULL;
    CHECK(xr_compile_resources_calloc(context->resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context->resources,module->function_count,sizeof(*owner->entries),
        (void **)&owner->entries)==XR_COMPILE_RESOURCE_OK);
    XrXirProgramProof proof=xr_xir_compile_program_proof(lowered);
    CHECK(xr_xir_compile_vm_program_take(&lowered,&owner->vm_program)==XR_XIR_OK && !lowered);
    CHECK(owner->vm_program->entry_count==module->function_count);
    unsigned native_count=0,vm_count=0;
    for(uint32_t f=0;f<module->function_count;++f) {
        bool native_entry=mode==1 || (mode==2 && f==tuple_destructure_make) ||
            (mode==3 && f!=tuple_destructure_make);
        owner->entries[f]=native_entry ? native->entries[f] : owner->vm_program->entries[f];
        CHECK((owner->entries[f].resume==native->entries[f].resume)==native_entry);
        if(native_entry)++native_count;else ++vm_count;
    }
    CHECK(native_count+vm_count==module->function_count);
    if(mode>=2)CHECK(native_count && vm_count);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,target,owner->entries,module->function_count,
        module->declarations,{owner,destructure_mixed_free},module->types,proof};
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(context,&spec,&program)==XR_XIR_OK && program);
    printf("Destructure backend mode%u: native=%u VM=%u; make role verified\n",mode,native_count,vm_count);
    return program;
}
static void destructure_mixed_execute(unsigned mode) {
    XrXirProgram *program=destructure_mixed_build(mode);
    if(mode>=2)destructure_runtime_faults(program,tuple_destructure_make);
    size_t base_live=runtime_live,base_bytes=runtime_bytes;
    for(unsigned run=0;run<2;++run) {
        XrXirInstanceConfig config;XrXirInstance *instance=NULL;XrXirValue result={0};unsigned groups=0;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,destructure_output,&groups};
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,tuple_destructure_make,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED && groups==1);
        CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        if(run==1) {
            xr_xir_compile_program_drop(program);
            CHECK(releases==mode+1);
        }
        destructure_retained(&result);
        if(run==0)CHECK(runtime_live==base_live && runtime_bytes==base_bytes);
    }
    CHECK(!runtime_live && !runtime_bytes);
}
int main(void) {
    for(unsigned mode=0;mode<4;++mode)destructure_mixed_execute(mode);
    CHECK(releases==4 && !runtime_live && !runtime_bytes);
    effects_source_owners_free();
    puts("Destructure four backend selections: fixed output, isolated instances, producer-first drop and physical release PASS");
    return 0;
}
