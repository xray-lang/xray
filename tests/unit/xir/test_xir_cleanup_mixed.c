/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cleanup_mixed.c - Source cleanup across both VM/native body boundaries
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_cleanup_source_cases.h"
XR_DATA const XrXirProgramSpec cleanup_source_program;
XR_DATA const uint32_t cleanup_source_functions[CLEANUP_SOURCE_FUNCTIONS];
XR_DATA const uint32_t cleanup_source_fatal_functions[4];
typedef struct CleanupMixedOwner {
    XrXirArtifact *artifact;XrXirProgram *vm_program;XrXirCallEntry *entries;
} CleanupMixedOwner;
static unsigned cleanup_mixed_releases;
static void cleanup_mixed_free(void *pointer) {
    CleanupMixedOwner *owner=pointer;
    xr_xir_compile_artifact_free(owner->artifact);
    xr_xir_compile_program_drop(owner->vm_program);
    xr_compile_resources_free(owner->entries);
    xr_compile_resources_free(owner);++cleanup_mixed_releases;
}
static XrXirStatus cleanup_mixed_allocate(const XrXirCompileContext *context,size_t count,size_t bytes,void **output) {
    XrCompileResourceStatus status=xr_compile_resources_calloc(context->resources,count,bytes,output);
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
}
static XrXirStatus cleanup_mixed_build(const XrXirCompileContext *context,unsigned native_cleanup,XrXirProgram **output) {
    CleanupMixedOwner *owner=NULL;XrXirArtifact *checked=NULL;
    XrXirStatus status=cleanup_mixed_allocate(context,1,sizeof(*owner),(void **)&owner);
    if(status!=XR_XIR_OK)return status;
    status=xr_xir_compile_checked_read(context,cleanup_source_program.proof.bytes,cleanup_source_program.proof.length,&checked,NULL);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(checked,&target,&owner->artifact,NULL);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module=status==XR_XIR_OK?xr_xir_compile_artifact_module(owner->artifact):NULL;
    if(status==XR_XIR_OK) {
        CHECK(module->function_count==cleanup_source_program.entry_count);
        status=cleanup_mixed_allocate(context,module->function_count,sizeof(*owner->entries),(void **)&owner->entries);
    }
    /* The sole immutable artifact stays owned by the actual sealed VM Program.
     * Its entries are borrowed only until the final mixed code lease releases it. */
    const XrXirArtifact *proof_artifact=owner->artifact;
    if(status==XR_XIR_OK)status=xr_xir_compile_vm_program_take(&owner->artifact,&owner->vm_program);
    unsigned native=0,vm=0;
    if(status==XR_XIR_OK) {
        CHECK(!owner->artifact && owner->vm_program->entry_count==module->function_count);
        CHECK(owner->vm_program->context.resources==context->resources);
        for(uint32_t f=0;f<module->function_count;++f) {
            bool cleanup=module->declarations->functions[f].cleanup_owner!=0;
            if(cleanup==(native_cleanup!=0)) {owner->entries[f]=cleanup_source_program.entries[f];++native;}
            else {owner->entries[f]=owner->vm_program->entries[f];++vm;}
        }
    }
    if(status==XR_XIR_OK) {
        CHECK(native && vm);
        XrXirProgramSpec spec=cleanup_source_program;spec.entries=owner->entries;
        spec.declarations=module->declarations;spec.types=module->types;
        spec.proof=xr_xir_compile_program_proof(proof_artifact);spec.code=(XrXirCodeLease){owner,cleanup_mixed_free};
        status=xr_xir_compile_program_seal(context,&spec,output);
    }
    if(status!=XR_XIR_OK){CHECK(!*output);cleanup_mixed_free(owner);}return status;
}
static void cleanup_source_mixed(bool native_cleanup,unsigned fatal_mode) {
    XrXirCompileContext context=cleanup_source_context((XrCompileResourceLimits){67108864,8388608,128000000});
    XrCompileResourceStats baseline=cleanup_source_stats(&context);XrXirProgram *program=NULL;
    XrXirStatus status=cleanup_mixed_build(&context,(unsigned)native_cleanup,&program);
    if(status!=XR_XIR_OK)fprintf(stderr,"cleanup mixed pipeline status%u direction%u\n",status,(unsigned)native_cleanup);
    CHECK(status==XR_XIR_OK && program);
    if(fatal_mode<4)cleanup_source_fatal(program,cleanup_source_fatal_functions[fatal_mode]);
    cleanup_source_primary_stats(&context,(unsigned)native_cleanup);
    cleanup_source_allocations(program,cleanup_source_functions);
    unsigned releases=cleanup_mixed_releases;
    cleanup_source_cases(program,cleanup_source_functions);program=NULL;
    CHECK(cleanup_mixed_releases==releases+1);
    cleanup_source_owner_free(&context,baseline);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==4 && !strcmp(argv[1],"--fatal")));
    if(argc==4) {
        unsigned mode=(unsigned)atoi(argv[2]);CHECK(mode<4);
        cleanup_source_mixed(atoi(argv[3])!=0,mode);
    }
    cleanup_source_mixed(false,4);cleanup_source_mixed(true,4);
    cleanup_source_compiler(cleanup_mixed_build,0);cleanup_source_compiler(cleanup_mixed_build,1);
    puts("Source cleanup in both VM/native directions matched independent expectations");return 0;
}
