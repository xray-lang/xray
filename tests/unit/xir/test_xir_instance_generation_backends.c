/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_instance_generation_backends.c - Real code snapshots over one owner
 */
#include "xir/xxir_instance_generation.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_instance_compile_observer.h"
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "root_execution_native_generated.h"
#if XR_GENERATION_OLD_VM || XR_GENERATION_NEW_VM
#include "xir/xxir_vm.h"
#include "xir_root_execution_native_pipeline.h"
#endif

static XrXirProgram *generation_program(const XrXirCompileContext *context,unsigned fixture,bool vm) {
    XrXirProgram *program=NULL;
#if XR_GENERATION_OLD_VM || XR_GENERATION_NEW_VM
    (void)rn_emit_fixture;
    if(vm) {
        XrXirArtifact *lowered=rn_lower_source(context,fixture);
        CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
        return program;
    }
#else
    CHECK(!vm);
#endif
    const XrXirProgramSpec *spec=fixture==0?&rn_identity_program:fixture==1?&rn_resume_program:&rn_init_failure_program;
    CHECK(xr_xir_compile_program_seal(context,spec,&program)==XR_XIR_OK);
    return program;
}
static XrXirInstanceResult generation_backend_poll(XrXirInstance *instance) {
    XrXirInstanceResult result;
    do{result=xr_xir_instance_poll_bounded(instance,256);}while(result.outcome.status==XR_XIR_CALL_READY);
    return result;
}
static int64_t generation_integer(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(generation_backend_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64);return value.payload;
}
int main(void) {
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    XrXirProgram *old=generation_program(&compiler.context,0,XR_GENERATION_OLD_VM);
    XrXirProgram *newer=generation_program(&compiler.context,1,XR_GENERATION_NEW_VM);
    XrXirProgram *failed=generation_program(&compiler.context,3,XR_GENERATION_NEW_VM);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_generation_new(&config,&instance)==XR_XIR_CALL_READY);
    uint64_t first=0;CHECK(xr_xir_instance_generation_prepare(instance,old,&first)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,rn_identity_ids[2],NULL,0)==XR_XIR_CALL_READY);
    CHECK(generation_backend_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue function={0};CHECK(xr_xir_instance_take_result(instance,&function)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_generation_publish(instance,first)==XR_XIR_CALL_READY);
    uint64_t second=0;CHECK(xr_xir_instance_generation_prepare(instance,newer,&second)==XR_XIR_CALL_READY);
    CHECK(generation_integer(instance,rn_resume_ids[1])==0);
    CHECK(xr_xir_instance_generation_publish(instance,second)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(old);xr_xir_compile_program_drop(newer);
    for(int64_t expected=42;expected<=44;expected+=2) {
        CHECK(xr_xir_instance_start_function(instance,&function,NULL,0)==XR_XIR_CALL_READY);
        CHECK(generation_backend_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && value.payload==expected);
    }
    CHECK(generation_integer(instance,rn_resume_ids[1])==0);
    uint64_t third=0;CHECK(xr_xir_instance_generation_prepare(instance,failed,&third)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,rn_init_failure_ids[0],NULL,0)==XR_XIR_CALL_READY);
    CHECK(generation_backend_poll(instance).outcome.status==XR_XIR_CALL_ASSERTION);
    XrXirCallResult escaped={0};CHECK(xr_xir_instance_copy_failure(instance,&escaped)==XR_XIR_CALL_ASSERTION);
    CHECK(xr_xir_instance_generation_publish(instance,third)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_generation_abort(instance,third)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(failed);
    CHECK(generation_integer(instance,rn_resume_ids[1])==0);
    CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start_function(instance,&function,NULL,0)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&escaped.panic.message,&bytes,&length) &&
        length==17 && !memcmp(bytes,"root-init-failure",17));
    xr_xir_call_result_drop(&escaped);xr_xir_value_drop(&function);
    CHECK(!runtime_live && !runtime_bytes);source_fixture_owner_free(&compiler);instance_compile_zero();
    printf("Generation real Source oldVM=%u newVM=%u: old closure42/44, new slot0, abort assertion, revoke and physical0 PASS\n",
        (unsigned)XR_GENERATION_OLD_VM,(unsigned)XR_GENERATION_NEW_VM);
    return 0;
}
