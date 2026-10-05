/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_array_execution.c - Real class identity execution and lifetime gates
 *
 * KEY CONCEPT:
 *   One Checked source graph feeds independent VM and native expected results.
 */
#include "xir/xxir_class.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define C(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#define CHECK(x) C(x)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_class_array_compile_owner.h"
#include "xir_class_array_contract_cases.h"
#include "xir_class_array_domain_cases.h"
extern const XrXirProgramSpec source_class_array_program;
static void class_runtime_faults(XrXirProgram *program) {
 size_t base=runtime_live,bytes=runtime_bytes,sites=0;
 for(size_t pass=0;pass<=sites;++pass){runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
 XrXirInstance *instance=NULL;XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);XrXirValue value={0};
 XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,3,NULL,0);
 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
 if(status==XR_XIR_CALL_RETURNED){C(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);C(value.type==XR_XIR_I64&&value.payload==41);xr_xir_value_drop(&value);
 status=xr_xir_instance_start(instance,4,NULL,0);if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
 if(status==XR_XIR_CALL_RETURNED)C(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);}
 if(!pass){C(status==XR_XIR_CALL_RETURNED);sites=runtime_attempts;C(sites>0);}else C(status==XR_XIR_CALL_OOM);
 if(instance)C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&value);
 C(runtime_live==base && runtime_bytes==bytes);
 }
 runtime_fail_at=SIZE_MAX;printf("class runtime OOM sites %zu physical baseline restored\n",sites);
}
#include "xir_class_array_retained.h"
typedef struct ClassMixedOwner {XrXirProgram *program;XrXirCallEntry *entries;} ClassMixedOwner;
static unsigned class_mixed_releases;
static void class_mixed_free(void *pointer) {
    ClassMixedOwner *owner=pointer;xr_xir_compile_program_drop(owner->program);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner);++class_mixed_releases;
}
static XrXirStatus class_allocate(const XrXirCompileContext *context,size_t count,size_t bytes,void **output) {
    XrCompileResourceStatus status=xr_compile_resources_calloc(context->resources,count,bytes,output);
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
}
static XrXirStatus class_execution_build(const XrXirCompileContext *context,unsigned mixed,XrXirProgram **output) {
    if(!mixed)return xr_xir_compile_program_seal(context,&source_class_array_program,output);
    ClassMixedOwner *owner=NULL;XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;
    XrXirStatus status=class_allocate(context,1,sizeof(*owner),(void **)&owner);
    if(status!=XR_XIR_OK)return status;
    status=xr_xir_compile_checked_read(context,source_class_array_program.proof.bytes,source_class_array_program.proof.length,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(checked,&special,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(special,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(special,&source_class_array_program.target,&lowered,NULL);
    const XrXirModule *module=status==XR_XIR_OK?xr_xir_compile_artifact_module(lowered):NULL;
    if(status==XR_XIR_OK){C(module->function_count==source_class_array_program.entry_count && module->function_count<=32);
        status=class_allocate(context,module->function_count,sizeof(*owner->entries),(void **)&owner->entries);}
    const XrXirArtifact *proof_artifact=lowered;
    if(status==XR_XIR_OK)status=xr_xir_compile_vm_program_take(&lowered,&owner->program);
    if(status==XR_XIR_OK) {
        C(!lowered && owner->program->context.resources==context->resources);unsigned native=0,vm=0;
        for(uint32_t f=0;f<module->function_count;++f) {
            if((f%2)==1){owner->entries[f]=source_class_array_program.entries[f];++native;}
            else {owner->entries[f]=owner->program->entries[f];C(owner->entries[f].resume!=source_class_array_program.entries[f].resume);++vm;}
        }
        C(native && vm);XrXirProgramSpec spec=source_class_array_program;spec.entries=owner->entries;
        spec.declarations=module->declarations;spec.types=module->types;spec.proof=xr_xir_compile_program_proof(proof_artifact);
        spec.code=(XrXirCodeLease){owner,class_mixed_free};status=xr_xir_compile_program_seal(context,&spec,output);
    }
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(special);xr_xir_compile_artifact_free(lowered);
    if(status!=XR_XIR_OK){C(!*output);class_mixed_free(owner);}return status;
}
static void class_program_attacks(const XrXirCompileContext *context) {
    const XrXirProgramSpec spec=source_class_array_program;XrXirProgram *program=NULL;
    class_array_contract_packet(context,spec.proof.bytes,spec.proof.length);
    for(uint8_t revision=47;revision<=51;++revision) {
        uint8_t *old_proof=class_array_old_contract(spec.proof.bytes,spec.proof.length,revision);
        uint8_t old_identity[32];xr_sha256(old_proof,spec.proof.length,old_identity);
        XrXirProgramSpec previous=spec;previous.proof.bytes=old_proof;previous.proof.identity=old_identity;
        size_t before_live=runtime_live,before_bytes=runtime_bytes;
        C(xr_xir_compile_program_seal(context,&previous,&program)==XR_XIR_BAD_STRUCTURE);
        C(!program && runtime_live==before_live && runtime_bytes==before_bytes);free(old_proof);
    }
    XrXirProgramSpec old=spec;old.abi_version=23;old.declarations=(const XrXirDeclarations *)(uintptr_t)1;
    C(xr_xir_compile_program_seal(context,&old,&program)==XR_XIR_BAD_LAYOUT && !program);
    old=spec;old.abi_version=24;old.declarations=(const XrXirDeclarations *)(uintptr_t)1;
    C(xr_xir_compile_program_seal(context,&old,&program)==XR_XIR_BAD_LAYOUT && !program);
    old=spec;old.target.abi_version=14;old.declarations=(const XrXirDeclarations *)(uintptr_t)1;
    C(xr_xir_compile_program_seal(context,&old,&program)==XR_XIR_BAD_LAYOUT && !program);
    old=spec;old.target.abi_version=13;old.declarations=(const XrXirDeclarations *)(uintptr_t)1;
    C(xr_xir_compile_program_seal(context,&old,&program)==XR_XIR_BAD_LAYOUT && !program);
    old=spec;old.target.abi_version=19;old.declarations=(const XrXirDeclarations *)(uintptr_t)1;
    C(xr_xir_compile_program_seal(context,&old,&program)==XR_XIR_BAD_LAYOUT && !program);
    uint8_t *packet=malloc(spec.proof.length);C(packet);
    for(unsigned mode=0;mode<2;++mode){memcpy(packet,spec.proof.bytes,spec.proof.length);packet[mode?12:8]=mode?46:18;
        XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,packet,32);xr_sha256_update(&sha,packet+64,spec.proof.length-64);xr_sha256_final(&sha,packet+32);
        XrXirArtifact *rejected=NULL;C(xr_xir_compile_checked_read(context,packet,spec.proof.length,&rejected,NULL)==XR_XIR_BAD_STRUCTURE && !rejected);}
    free(packet);program=(XrXirProgram *)(uintptr_t)1;size_t before=instance_compile_attempts;
    C(xr_xir_compile_program_seal(context,&spec,&program)==XR_XIR_BAD_STRUCTURE);
    C(program==(XrXirProgram *)(uintptr_t)1 && instance_compile_attempts==before);
}
int main(void) {
    class_array_arena_faults();
    XrXirCompileContext independent=class_array_context(class_array_limits());XrCompileResourceStats separate=class_array_stats(&independent);
    class_array_independent_cases(&independent);class_array_retained_independent(&independent);class_array_owner_free(&independent,separate);
    const int argc=XR_CLASS_ARRAY_MIXED?2:1;
    XrXirCompileContext context=class_array_context(class_array_limits());XrCompileResourceStats baseline=class_array_stats(&context);
    class_program_attacks(&context);XrXirProgram *program=NULL;
    C(class_execution_build(&context,XR_CLASS_ARRAY_MIXED,&program)==XR_XIR_OK);
    class_array_pipeline_report(&context,argc>1?"mixed":"native");unsigned releases=class_mixed_releases;
 class_runtime_faults(program);
 XrXirValue saved[2]={{0},{0}};
 for(unsigned i=0;i<2;++i){XrXirInstance *instance=NULL;XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
 C(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 C(xr_xir_instance_start(instance,3,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
 XrXirValue number={0};C(xr_xir_instance_take_result(instance,&number)==XR_XIR_CALL_RETURNED);C(number.type==XR_XIR_I64 && number.payload==41);xr_xir_value_drop(&number);
 C(xr_xir_instance_start(instance,4,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
 C(xr_xir_instance_take_result(instance,&saved[i])==XR_XIR_CALL_RETURNED);C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);}
 class_array_suspend_cases(program);xr_xir_compile_program_drop(program);
 for(unsigned i=0;i<2;++i)class_array_retained(&saved[i]);
 C(!runtime_live && !runtime_bytes);
    C(class_mixed_releases==releases+(XR_CLASS_ARRAY_MIXED?1u:0u));class_array_owner_free(&context,baseline);
    class_array_compiler_faults(class_execution_build,XR_CLASS_ARRAY_MIXED);
    puts(argc>1?"class Array mixed41 retained Array/string oldABI PASS":"class Array native41 retained Array/string oldABI PASS");return 0;
}
