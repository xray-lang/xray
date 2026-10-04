/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_nested_nullable_execution.c - Actual Source native and two mixed directions
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_source_nested_nullable_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_source_nested_nullable_cases.h"
extern const XrXirProgramSpec source_nested_nullable_program;
static void sn_execution(bool direction) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirProgramSpec spec=source_nested_nullable_program;XrXirArtifact *proof=NULL,*lowered=NULL;
    CHECK(spec.proof.bytes && spec.proof.identity && spec.types && spec.declarations && spec.entry_count);
    CHECK(xr_xir_compile_checked_read(&owner.context,spec.proof.bytes,spec.proof.length,&proof,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(proof);uint32_t functions[SN_COUNT];sn_find(module,functions);
    CHECK(module->function_count==spec.entry_count && module->declarations->entry_function==spec.declarations->entry_function);
    XrXirCallEntry *entries=NULL;XrXirVmBinding *bindings=NULL;
    if (XR_SOURCE_NESTED_MIXED) {
        CHECK(xr_xir_compile_lower(proof,&spec.target,&lowered,NULL)==XR_XIR_OK);
        CHECK(xr_compile_resources_calloc(owner.context.resources,spec.entry_count,sizeof(*entries),(void**)&entries)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_resources_calloc(owner.context.resources,spec.entry_count,sizeof(*bindings),(void**)&bindings)==XR_COMPILE_RESOURCE_OK);
        uint32_t vm=0,native=0;
        for (uint32_t f=0;f<spec.entry_count;++f) {
            CHECK(xr_xir_compile_vm_bind(lowered,f,&bindings[f],&entries[f])==XR_XIR_OK);
            /* Exported callers and generic helper callees cross in both directions. */
            bool selected=false;
            for (uint32_t n=0;n<SN_COUNT;++n) if (functions[n]==f) selected=true;
            bool use_vm=selected?direction:!direction;
            if (use_vm) {CHECK(entries[f].resume!=spec.entries[f].resume);++vm;}
            else {entries[f]=spec.entries[f];++native;}
        }
        CHECK(vm && native);module=xr_xir_compile_artifact_module(lowered);
        spec.entries=entries;spec.types=module->types;spec.declarations=module->declarations;
        XrXirProgramProof matching=xr_xir_compile_program_proof(lowered);
        CHECK(matching.identity && matching.length==spec.proof.length && !memcmp(matching.identity,spec.proof.identity,32) &&
            !memcmp(matching.bytes,spec.proof.bytes,matching.length));spec.proof=matching;
    }
    xr_xir_compile_artifact_free(proof);
    XrXirProgram *program=NULL;XrXirProgramSpec wrong=spec;
    wrong.abi_version=XR_XIR_PROGRAM_ABI_VERSION-1;
    CHECK(xr_xir_compile_program_seal(&owner.context,&wrong,&program)==XR_XIR_BAD_LAYOUT && !program);
    wrong=spec;wrong.target.abi_version=XR_XIR_VALUE_ABI_VERSION-1;
    CHECK(xr_xir_compile_program_seal(&owner.context,&wrong,&program)==XR_XIR_BAD_LAYOUT && !program);
    CHECK(xr_xir_compile_program_seal(&owner.context,&spec,&program)==XR_XIR_OK);
    XrXirValue held[6]={{0}};sn_program_cases(program,functions,held);
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);
    xr_compile_resources_free(bindings);xr_compile_resources_free(entries);
    sn_retained(held);CHECK(!runtime_live && !runtime_bytes);source_nested_compile_owner_free(&owner);
}
int main(void) {
    sn_execution(false);if (XR_SOURCE_NESTED_MIXED) sn_execution(true);
    puts(XR_SOURCE_NESTED_MIXED?"Source nested two real mixed directions physical PASS":"Source nested actual native fixed golden physical PASS");return 0;
}
