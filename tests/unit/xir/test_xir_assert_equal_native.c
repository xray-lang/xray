/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_assert_equal_native.c - Actual native and mixed typed equality outcomes
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_source_query.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec equal_checked_program;
XR_DATA const uint32_t equal_checked_functions[13];
XR_DATA const uint32_t equal_checked_inputs[2];
#include "xir_assert_equal_inputs.h"
static unsigned equal_native_releases;
typedef struct EqualNativeOwner {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;} EqualNativeOwner;
static void equal_native_release(void *pointer) {
    EqualNativeOwner *owner=pointer;xr_xir_artifact_free(owner->artifact);
    xr_free(owner->entries);xr_free(owner->bindings);xr_free(owner);++equal_native_releases;
}
static void equal_native_cases(XrXirProgram *program) {
    const int64_t expected[]={1,7,3,5,6,7,0,3,123,11,12,13,14};
    equal_instance_oom(program,equal_checked_functions[2]);
    XrXirInstance *instance=NULL;XrXirInstanceConfig config=xr_xir_instance_defaults();
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_program_drop(program);
    for (uint32_t i=0;i<13;++i) {
        CHECK(xr_xir_instance_start(instance,equal_checked_functions[i],NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult polled=xr_xir_instance_poll(instance);
        CHECK(polled.outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && (int64_t)value.payload==expected[i]);xr_xir_value_drop(&value);
    }
    XrXirValue held[2]={{0}};
    equal_owned_inputs(instance,equal_checked_inputs[0],equal_checked_inputs[1],held);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);equal_held_messages(held);
}
static void equal_native_mixed(const XrXirProgramSpec *native,bool root_native) {
    XrXirArtifact *checked=NULL;CHECK(xr_xir_checked_read(native->proof.bytes,native->proof.length,NULL,&checked,NULL)==XR_XIR_OK);
    EqualNativeOwner *owner=xr_calloc(1,sizeof(*owner));CHECK(owner);
    CHECK(xr_xir_lower(checked,&native->target,NULL,&owner->artifact,NULL)==XR_XIR_OK);xr_xir_artifact_free(checked);
    const XrXirModule *module=xr_xir_artifact_module(owner->artifact);CHECK(module->function_count==native->entry_count);
    owner->entries=xr_calloc(module->function_count,sizeof(*owner->entries));
    owner->bindings=xr_calloc(module->function_count,sizeof(*owner->bindings));CHECK(owner->entries && owner->bindings);
    unsigned native_count=0,vm_count=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        CHECK(xr_xir_vm_bind(owner->artifact,f,&owner->bindings[f],&owner->entries[f])==XR_XIR_OK);
        bool root=module->declarations->functions[f].module==module->declarations->root_module;
        if (root==root_native) {owner->entries[f]=native->entries[f];++native_count;} else ++vm_count;
    }
    CHECK(native_count && vm_count);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,native->target,owner->entries,module->function_count,
        module->declarations,{owner,equal_native_release},module->types,xr_xir_program_proof(owner->artifact)};
    XrXirProgram *program=NULL;CHECK(xr_xir_program_seal(&spec,(XrXirProgramBudget){16777216,64000000},&program)==XR_XIR_OK);
    equal_native_cases(program);
}
int main(void) {
    _Static_assert(sizeof(XrXirGeneric)==40 && sizeof(XrXirSourceDeclaration)==120,"internal result-role metadata requires fresh consumers");
    _Static_assert(XR_XIR_VALUE_ABI_VERSION==16 && sizeof(XrXirValue)==16,"public value ABI");
    _Static_assert(XR_XIR_CALL_ABI_VERSION==20 && sizeof(XrXirAction)==88 && sizeof(XrXirCallResult)==72 &&
        sizeof(XrXirCallView)==216 && sizeof(XrXirCallEntry)==64,"public call ABI");
    _Static_assert(XR_XIR_PROGRAM_ABI_VERSION==26 && sizeof(XrXirProgramSpec)==96,"public program ABI");
    const XrXirProgramSpec *spec=&equal_checked_program;
    XrXirProgram *program=NULL;CHECK(xr_xir_program_seal(spec,(XrXirProgramBudget){16777216,64000000},&program)==XR_XIR_OK);
    equal_native_cases(program);CHECK(!runtime_live && !runtime_bytes);
    equal_native_mixed(spec,false);CHECK(!runtime_live && !runtime_bytes);
    equal_native_mixed(spec,true);CHECK(!runtime_live && !runtime_bytes);
    CHECK(equal_native_releases==2);
    puts("13 independent outcomes: native and both VM/native directions PASS; Value16 Call20 Program26 unchanged; physical baseline restored");return 0;
}
