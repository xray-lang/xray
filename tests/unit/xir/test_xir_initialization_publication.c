/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_initialization_publication.c - Canonical failure publication at retain saturation
 *
 * KEY CONCEPT:
 *   Inject saturation only after a typed Call result exists. No language code,
 *   second initializer or invented error layout is needed for this boundary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir/xxir_error.h"
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_runtime_allocations.h"
#undef CHECK
#define main original_instance_regressions
#include "test_xir_instances.c"
#undef main
static XrXirAction ordinary_throw(XrXirCallView *view) {
    Frame *frame=view->state;frame->value=error_fixture_code(xr_xir_call_admission(view),91);
    return action(XR_XIR_ACTION_THROW,frame->value);
}
static void ordinary_throw_take(void) {
    Fixture data;fixture(&data,2);data.witness.mode=0;data.entries[3].resume=ordinary_throw;
    XrXirProgram *program=NULL;
    CHECK(xr_xir_program_seal(&data.spec,(XrXirProgramBudget){2097152,16000000},&program)==XR_XIR_OK);
    xr_xir_artifact_free(data.proof);Trace log={0};XrXirInstance *instance=new_instance(program,&log);
    CHECK(xr_xir_instance_start(instance,3,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_THROWN);
    CHECK(xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY);
    XrXirValue error={0};CHECK(xr_xir_instance_take_result(instance,&error)==XR_XIR_CALL_THROWN);
    XrXirValue number=run(instance,4);CHECK(number.type==XR_XIR_I64&&number.payload==10);xr_xir_value_drop(&number);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_program_drop(program);
    XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
    CHECK(error_fixture_is_code(&error,reader,91));xr_xir_value_drop(&error);xr_xir_domain_drop(reader);
    CHECK(!runtime_live&&!runtime_bytes);
}
int main(void) {
    CHECK(original_instance_regressions()==0);CHECK(!runtime_live&&!runtime_bytes);
    Fixture fixture_data;fixture(&fixture_data,2);XrXirProgram *program=NULL;
    CHECK(xr_xir_program_seal(&fixture_data.spec,(XrXirProgramBudget){2097152,16000000},&program)==XR_XIR_OK);
    xr_xir_artifact_free(fixture_data.proof);fixture_data.proof=NULL;
    Trace trace_data={0};XrXirInstance *instance=new_instance(program,&trace_data);
    CHECK(xr_xir_instance_start(instance,3,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult paused=xr_xir_instance_poll(instance);CHECK(paused.outcome.status==XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_resume(instance,paused.epoch,paused.outcome.wake)==XR_XIR_CALL_READY);
    /* Execute the real active callback driver, then inject at the exact gap
     * before Instance copies/publishes its terminal result. */
    instance->driving=true;XrXirCallResult terminal=xr_xir_call_poll(instance->call);instance->driving=false;
    CHECK(terminal.status==XR_XIR_CALL_THROWN&&instance->state==XR_XIR_INSTANCE_INITIALIZING);
    XirObject *object=object_pointer(&terminal.value);uint32_t references=atomic_load(&object->references);
    CHECK(references==1);atomic_store(&object->references,UINT32_MAX);
    XrXirInstanceResult first=xr_xir_instance_poll(instance),again=xr_xir_instance_poll(instance);
    printf("first=%u cached=%u state=%u epoch=%llu\n",first.outcome.status,again.outcome.status,instance->state,(unsigned long long)first.epoch);fflush(stdout);
    CHECK(first.outcome.status==XR_XIR_CALL_LIMIT);
    CHECK(again.outcome.status==XR_XIR_CALL_LIMIT&&instance->state==XR_XIR_INSTANCE_FAILED);
    CHECK(first.epoch==paused.epoch&&again.epoch==paused.epoch&&!instance->publication_count);
    CHECK(!again.outcome.value.type&&!again.outcome.value.payload&&!again.outcome.wake&&xr_xir_fault_empty(again.outcome.panic.detail));
    CHECK(xr_xir_instance_start(instance,3,NULL,0)==XR_XIR_CALL_LIMIT);
    XrXirCallResult copy={0};CHECK(xr_xir_instance_copy_failure(instance,&copy)==XR_XIR_CALL_LIMIT);
    CHECK(copy.status==XR_XIR_CALL_LIMIT&&!copy.value.type&&!copy.value.payload&&xr_xir_fault_empty(copy.panic.detail));
    atomic_store(&object->references,references);
    XrXirValue taken={XR_XIR_I64,0,73},before_take=taken;
    CHECK(xr_xir_instance_take_result(instance,&taken)==XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&taken,&before_take,sizeof(taken)));
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    /* A second real initialization publishes an ordinary owned failure. */
    trace_data=(Trace){0};instance=new_instance(program,&trace_data);
    CHECK(xr_xir_instance_start(instance,3,NULL,0)==XR_XIR_CALL_READY);
    paused=xr_xir_instance_poll(instance);CHECK(paused.outcome.status==XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_resume(instance,paused.epoch,paused.outcome.wake)==XR_XIR_CALL_READY);
    first=xr_xir_instance_poll(instance);CHECK(first.outcome.status==XR_XIR_CALL_THROWN);
    object=object_pointer(&first.outcome.value);
    references=atomic_load(&object->references);atomic_store(&object->references,UINT32_MAX);
    copy=(XrXirCallResult){0};XrXirCallResult unchanged=copy;
    CHECK(xr_xir_instance_copy_failure(instance,&copy)==XR_XIR_CALL_LIMIT);
    CHECK(!memcmp(&copy,&unchanged,sizeof(copy))&&instance->failure.status==XR_XIR_CALL_THROWN);
    atomic_store(&object->references,references);
    CHECK(xr_xir_instance_copy_failure(instance,&copy)==XR_XIR_CALL_THROWN);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_program_drop(program);
    XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
    CHECK(error_fixture_is_code(&copy.value,reader,91));xr_xir_domain_drop(reader);
    size_t attempts=runtime_attempts;runtime_fail_at=attempts;xr_xir_value_drop(&copy.value);
    CHECK(runtime_attempts==attempts&&!runtime_live&&!runtime_bytes);
    runtime_fail_at=SIZE_MAX;ordinary_throw_take();
    puts("fixed canonical failure, READY ordinary throw take and owned-copy physical release PASS");
    return 0;
}
