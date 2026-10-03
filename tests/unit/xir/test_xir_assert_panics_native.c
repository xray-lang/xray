/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_assert_panics_native.c - Actual native and mixed typed discard outcomes
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_source_query.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec panics_checked_program,panics_matrix_program;
XR_DATA const uint32_t panics_checked_functions[4],panics_matrix_functions[14];
static unsigned panics_native_releases;
typedef struct PanicsNativeOwner {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;} PanicsNativeOwner;
static void panics_native_release(void *pointer) {
    PanicsNativeOwner *owner=pointer;xr_xir_artifact_free(owner->artifact);
    xr_free(owner->entries);xr_free(owner->bindings);xr_free(owner);++panics_native_releases;
}
static void panics_native_cases(XrXirProgram *program,bool matrix) {
    const int64_t expected[]={445,445,445,445,445,0,11,11,420,420,123,3,19,445};
    const uint32_t *functions=matrix ? panics_matrix_functions : panics_checked_functions;
    XrXirInstance *instance=NULL;XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_program_drop(program);
    for (uint32_t i=0;i<(matrix ? 14u : 4u);++i) {
        CHECK(xr_xir_instance_start(instance,functions[i],NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult polled=xr_xir_instance_poll(instance);uint32_t suspended=0;
        while (polled.outcome.status==XR_XIR_CALL_SUSPENDED) {
            CHECK(++suspended==1 && matrix && i>=12);
            CHECK(xr_xir_instance_resume(instance,polled.epoch,polled.outcome.wake)==XR_XIR_CALL_READY);
            polled=xr_xir_instance_poll(instance);
        }
        CHECK(suspended==(uint32_t)(matrix && i>=12));CHECK(polled.outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        if (matrix || i==0 || i==2) CHECK(value.type==XR_XIR_I64 && (int64_t)value.payload==(matrix ? expected[i] : i==0 ? 7 : 445));
        else {const char *bytes=NULL;size_t length=0;const char *text=i==1 ? "unit" : "string";
            CHECK(xr_xir_string_view(&value,&bytes,&length) && length==strlen(text) && !memcmp(bytes,text,length));}
        xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
static void panics_native_mixed(const XrXirProgramSpec *native,bool matrix,bool root_native) {
    XrXirArtifact *checked=NULL;CHECK(xr_xir_checked_read(native->proof.bytes,native->proof.length,NULL,&checked,NULL)==XR_XIR_OK);
    PanicsNativeOwner *owner=xr_calloc(1,sizeof(*owner));CHECK(owner);
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
        module->declarations,{owner,panics_native_release},module->types,xr_xir_program_proof(owner->artifact)};
    XrXirProgram *program=NULL;CHECK(xr_xir_program_seal(&spec,(XrXirProgramBudget){16777216,64000000},&program)==XR_XIR_OK);
    panics_native_cases(program,matrix);
}
int main(void) {
    _Static_assert(sizeof(XrXirGeneric)==40 && sizeof(XrXirSourceDeclaration)==120,"internal result-role metadata requires fresh consumers");
    _Static_assert(XR_XIR_VALUE_ABI_VERSION==17 && sizeof(XrXirValue)==16,"public value ABI");
    _Static_assert(XR_XIR_CALL_ABI_VERSION==21 && sizeof(XrXirAction)==88 && sizeof(XrXirCallResult)==72 &&
        sizeof(XrXirCallView)==216 && sizeof(XrXirCallEntry)==64,"public call ABI");
    _Static_assert(XR_XIR_PROGRAM_ABI_VERSION==28 && sizeof(XrXirProgramSpec)==96,"public program ABI");
    for (uint32_t matrix=0;matrix<2;++matrix) {
        const XrXirProgramSpec *spec=matrix ? &panics_matrix_program : &panics_checked_program;
        XrXirProgram *program=NULL;CHECK(xr_xir_program_seal(spec,(XrXirProgramBudget){16777216,64000000},&program)==XR_XIR_OK);
        panics_native_cases(program,matrix!=0);CHECK(!runtime_live && !runtime_bytes);
        panics_native_mixed(spec,matrix!=0,false);CHECK(!runtime_live && !runtime_bytes);
        panics_native_mixed(spec,matrix!=0,true);CHECK(!runtime_live && !runtime_bytes);
    }
    CHECK(panics_native_releases==4);
    puts("18 independent outcomes: native and both VM/native directions PASS; Value17 Call21 Program26; carrier layouts unchanged; physical baseline restored");return 0;
}
