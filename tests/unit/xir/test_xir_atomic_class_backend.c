/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_atomic_class_backend.c - Class-held Atomic identities across native and VM calls
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_library_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_atomic_class_execution.h"
#include "xir_atomic_class_runtime_faults.h"
extern const XrXirProgramSpec atomic_class_program;
extern const uint32_t atomic_class_ids[2];
typedef struct AtomicClassCode {
    XrXirArtifact *artifact;
    XrXirVmBinding *bindings;
    XrXirCallEntry *entries;
} AtomicClassCode;
static void atomic_class_code_release(void *opaque) {
    AtomicClassCode *code=opaque;
    xr_compile_resources_free(code->entries);xr_compile_resources_free(code->bindings);
    xr_xir_compile_artifact_free(code->artifact);xr_compile_resources_free(code);
}
static XrXirStatus atomic_class_backend(const XrXirCompileContext *context,unsigned direction,XrXirProgram **output) {
    if (!direction) return xr_xir_compile_program_seal(context,&atomic_class_program,output);
    AtomicClassCode *code=NULL;XrXirArtifact *read=NULL,*special=NULL;
    XrCompileResourceStatus allocated=xr_compile_resources_calloc(context->resources,1,sizeof(*code),(void **)&code);
    if (allocated!=XR_COMPILE_RESOURCE_OK) return allocated==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirStatus status=xr_xir_compile_checked_read(context,atomic_class_program.proof.bytes,atomic_class_program.proof.length,&read,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(read,&special,NULL);
    xr_xir_compile_artifact_free(read);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(special,&atomic_class_program.target,&code->artifact,NULL);
    xr_xir_compile_artifact_free(special);
    if (status==XR_XIR_OK) status=xr_xir_compile_vm_bind_table(code->artifact,&code->bindings,&code->entries);
    if (status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(code->artifact);
        CHECK(module->function_count==atomic_class_program.entry_count);
        unsigned vm=0,native=0;
        for (uint32_t f=0;f<module->function_count;++f) {
            bool root=f==atomic_class_ids[0] || f==atomic_class_ids[1];
            bool use_native=direction==1 ? root : !root;
            if (use_native) {code->entries[f]=atomic_class_program.entries[f];++native;}
            else {CHECK(code->entries[f].resume!=atomic_class_program.entries[f].resume);++vm;}
        }
        CHECK(vm && native);XrXirProgramSpec spec=atomic_class_program;
        spec.entries=code->entries;spec.declarations=module->declarations;spec.types=module->types;
        spec.proof=xr_xir_compile_program_proof(code->artifact);spec.code=(XrXirCodeLease){code,atomic_class_code_release};
        status=xr_xir_compile_program_seal(context,&spec,output);
        if (status==XR_XIR_OK) code=NULL;
    }
    if (code) atomic_class_code_release(code);
    return status;
}
int main(int argc,char **argv) {
    CHECK((argc==2 || argc==3) && argv[1][1]=='\0' && argv[1][0]>='0' && argv[1][0]<='2');
    CHECK(argc==2 || !strcmp(argv[2],"--runtime-faults"));
    unsigned direction=(unsigned)(argv[1][0]-'0');LibraryCompileOwner owner={0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrXirProgram *program=NULL;CHECK(atomic_class_backend(&owner.context,direction,&program)==XR_XIR_OK && program);
    if (argc==3) atomic_class_runtime_faults(program,atomic_class_ids);
    else atomic_class_execute(program,atomic_class_ids);
    library_compile_owner_drop(&owner);library_compile_observer_free();CHECK(!runtime_live && !runtime_bytes);
    printf("Atomic Class native/mixed direction=%u and retained ownership PASS\n",direction);return 0;
}
