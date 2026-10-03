/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_nullable_native.c - Actual typed sums across native and VM entries
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec nullable_checked_program;
typedef struct NullableNativeOwner {
    XrXirArtifact *artifact;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} NullableNativeOwner;
static unsigned nullable_native_releases;
static void nullable_native_release(void *pointer) {
    NullableNativeOwner *owner=pointer;xr_xir_artifact_free(owner->artifact);
    xr_free(owner->entries);xr_free(owner->bindings);xr_free(owner);++nullable_native_releases;
}
static void nullable_native_failures(XrXirProgram *program,uint32_t entry) {
    size_t sites=0,live=runtime_live,bytes=runtime_bytes;
    for (size_t point=0;point<=sites;++point) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;XrXirInstance *instance=NULL;
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,entry,NULL,0);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll(instance).outcome.status;
        if (!point) {CHECK(status==XR_XIR_CALL_RETURNED);sites=runtime_attempts;CHECK(sites>0);}
        else {
            if (status!=XR_XIR_CALL_OOM) fprintf(stderr,"Nullable native fault=%zu/%zu status=%u\n",point-1,sites,status);
            CHECK(runtime_attempts>runtime_fail_at && status==XR_XIR_CALL_OOM);
        }
        runtime_fail_at=SIZE_MAX;
        if (instance) CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    printf("Nullable actual native/mixed runtime OOM sites=%zu physical refunds PASS\n",sites);
}
static void nullable_native_run(const XrXirProgramSpec *spec) {
    XrXirProgram *program=NULL;
    CHECK(xr_xir_program_seal(spec,(XrXirProgramBudget){16777216,64000000},&program)==XR_XIR_OK);
    nullable_native_failures(program,spec->declarations->entry_function);
    XrXirInstance *instance=NULL;XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_program_drop(program);
    CHECK(xr_xir_instance_start(instance,spec->declarations->entry_function,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallResult result=xr_xir_instance_poll(instance).outcome;
    if (result.status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"Nullable native status=%u\n",result.status);
    CHECK(result.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && !value.payload);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
static void nullable_native_mixed(const XrXirProgramSpec *native,bool root_native) {
    XrXirArtifact *checked=NULL;
    CHECK(xr_xir_checked_read(native->proof.bytes,native->proof.length,NULL,&checked,NULL)==XR_XIR_OK);
    NullableNativeOwner *owner=xr_calloc(1,sizeof(*owner));CHECK(owner);
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
        module->declarations,{owner,nullable_native_release},module->types,xr_xir_program_proof(owner->artifact)};
    nullable_native_run(&spec);
}
int main(void) {
    _Static_assert(XR_XIR_VALUE_ABI_VERSION==17 && XR_XIR_CALL_ABI_VERSION==21 &&
        XR_XIR_PROGRAM_ABI_VERSION==28,"sum values require current consumers");
    const XrXirProgramSpec *spec=&nullable_checked_program;XrXirProgramSpec old=*spec;
    old.target.abi_version=16;XrXirProgram *program=(XrXirProgram *)(uintptr_t)1;
    size_t before=runtime_attempts;
    CHECK(xr_xir_program_seal(&old,(XrXirProgramBudget){16777216,64000000},&program)==XR_XIR_BAD_LAYOUT);
    CHECK(!program && runtime_attempts==before && !runtime_live && !runtime_bytes);
    nullable_native_run(spec);CHECK(!runtime_live && !runtime_bytes);
    nullable_native_mixed(spec,false);CHECK(!runtime_live && !runtime_bytes);
    nullable_native_mixed(spec,true);CHECK(!runtime_live && !runtime_bytes && nullable_native_releases==2);
    puts("Nullable actual native and both VM/native directions; old Value16 refusal; physical baseline PASS");return 0;
}
