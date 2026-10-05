/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_assert_condition_native.c - Native and mixed intrinsic execution
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_assert_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_assert_condition_cases.h"
XR_DATA const XrXirProgramSpec assert_condition_program;
XR_DATA const uint32_t assert_condition_functions[ASSERT_FUNCTIONS];
typedef struct AssertMixed {
    XrXirArtifact *artifact;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} AssertMixed;
static unsigned assert_releases;
static void assert_mixed_free(void *pointer) {
    AssertMixed *owner=pointer;
    xr_xir_compile_artifact_free(owner->artifact);xr_free(owner->entries);xr_free(owner->bindings);xr_free(owner);
    ++assert_releases;
}
static void assert_mixed(bool root_native) {
    XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_checked_read(assert_compile_context,assert_condition_program.proof.bytes,assert_condition_program.proof.length,&checked,NULL)==XR_XIR_OK);
    AssertMixed *owner=xr_calloc(1,sizeof(*owner));CHECK(owner);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked,&target,&owner->artifact,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(owner->artifact);
    CHECK(module->function_count==assert_condition_program.entry_count);
    owner->entries=xr_calloc(module->function_count,sizeof(*owner->entries));
    owner->bindings=xr_calloc(module->function_count,sizeof(*owner->bindings));
    CHECK(owner->entries && owner->bindings);
    unsigned native=0,vm=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        CHECK(xr_xir_compile_vm_bind(owner->artifact,f,&owner->bindings[f],&owner->entries[f])==XR_XIR_OK);
        bool root=module->declarations->functions[f].module==module->declarations->root_module;
        if (root==root_native) {owner->entries[f]=assert_condition_program.entries[f];++native;}
        else ++vm;
    }
    CHECK(native && vm);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,target,owner->entries,module->function_count,
        module->declarations,{owner,assert_mixed_free},module->types,xr_xir_compile_program_proof(owner->artifact)};
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(assert_compile_context,&spec,&program)==XR_XIR_OK);
    assert_cases(program,assert_condition_functions);
}
int main(void) {
    assert_compile_begin();
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(assert_compile_context,&assert_condition_program,&program)==XR_XIR_OK);
    assert_cases(program,assert_condition_functions);
    CHECK(!runtime_live && !runtime_bytes);
    assert_mixed(false);CHECK(assert_releases==1 && !runtime_live && !runtime_bytes);
    assert_mixed(true);CHECK(assert_releases==2 && !runtime_live && !runtime_bytes);
    puts("Real generated native and both VM/native assertion directions PASS with exact bytes and physical OOM cleanup");
    assert_compile_end();return 0;
}
