/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_call_native.c - Independent native call and suspension expectations
 *
 * KEY CONCEPT:
 *   Only emitted C and the call runtime participate; no interpreter is linked.
 */
#include "xir/xxir_call.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
static void call_runtime_retiring(void *pointer);
#define XIR_RUNTIME_BEFORE_FREE(pointer) call_runtime_retiring(pointer)
#include "xir_runtime_allocations.h"
#undef XIR_RUNTIME_BEFORE_FREE
#include "xir_call_instance_owner.h"
#include "xir_error_fixture.h"
/* These descriptors contain the emitted Checked proof and actual native
 * entries. Every run seals that proof before it creates an Instance. */
XR_DATA const XrXirProgramSpec fixture_calls0_program;
XR_DATA const XrXirProgramSpec fixture_calls1_program;
XR_DATA const XrXirProgramSpec fixture_calls2_program;
XR_DATA const XrXirProgramSpec fixture_calls3_program;
XR_DATA const XrXirProgramSpec fixture_calls4_program;
XR_DATA const XrXirProgramSpec fixture_calls5_program;
XR_DATA const XrXirProgramSpec fixture_calls6_program;
XR_DATA const XrXirProgramSpec fixture_calls7_program;
XR_DATA const XrXirProgramSpec fixture_calls8_program;
XR_DATA const XrXirProgramSpec fixture_calls9_program;
XR_DATA const XrXirProgramSpec fixture_calls10_program;
XR_DATA const XrXirProgramSpec fixture_calls11_program;
XR_DATA const XrXirProgramSpec fixture_calls12_program;
XR_DATA const XrXirProgramSpec fixture_calls13_program;

static bool native_program_new(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec,XrXirProgram **program) {
    XrXirStatus status=xr_xir_compile_program_seal(context,spec,program);
    if (status!=XR_XIR_OK) {CHECK(status==XR_XIR_OUT_OF_MEMORY && !*program);return false;}
    return true;
}
static XrXirCallStatus native_instance_new(XrXirProgram *program,
    const XrXirOutputProvider *output,XrXirInstance **instance) {
    XrXirInstanceConfig config;call_instance_config(&config);
    if (output) config.output=*output;
    return xr_xir_instance_new(program,&config,instance);
}
static void native_instance_retire(XrXirInstance *instance,bool accepted,
    XrXirCallStatus status) {
    if (!instance) return;
    XrXirCallStatus expected=XR_XIR_CALL_READY;
    if (accepted && (status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT ||
        status==XR_XIR_CALL_BAD_STATE)) expected=status;
    CHECK(call_instance_retire(instance)==expected);
}
static bool native_invoke_allocation_run(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec,uint32_t mode,uint32_t cancel_at) {
    XrCompileResourceStats baseline={0};
    CHECK(xr_compile_resources_stats(context->resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;XrXirInstance *instance=NULL;XrXirValue owned={0};
    bool completed=false,accepted=false;XrXirCallStatus status=XR_XIR_CALL_OOM;
    if (!native_program_new(context,spec,&program)) goto done;
    status=native_instance_new(program,NULL,&instance);
    if (status!=XR_XIR_CALL_READY) {CHECK(status==XR_XIR_CALL_OOM);goto done;}
    xr_xir_compile_program_drop(program);program=NULL;
    XrXirValue args[]={{XR_XIR_I64,0,9},{XR_XIR_I64,0,4}};
    status=xr_xir_instance_start(instance,0,args,2);
    if (status!=XR_XIR_CALL_READY) {CHECK(status==XR_XIR_CALL_OOM);goto done;}
    accepted=true;
    XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    uint32_t suspensions=0;
    while (polled.outcome.status==XR_XIR_CALL_SUSPENDED) {
        CHECK(++suspensions<=2);
        if (suspensions==cancel_at) {
            CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
            CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_CANCELLED);
        } else CHECK(xr_xir_instance_resume(instance,polled.epoch,polled.outcome.wake)==XR_XIR_CALL_READY);
        polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    }
    status=polled.outcome.status;
    if (status==XR_XIR_CALL_OOM) goto done;
    if (cancel_at && suspensions==cancel_at) CHECK(status==XR_XIR_CALL_CANCELLED);
    else if (mode==9) {
        CHECK(status==XR_XIR_CALL_THROWN);
        CHECK(xr_xir_instance_take_result(instance,&owned)==XR_XIR_CALL_THROWN);
    } else CHECK(status==XR_XIR_CALL_RETURNED && polled.outcome.value.type==XR_XIR_I64 &&
        polled.outcome.value.payload==(mode==10?44:77));
    completed=true;
 done:
    native_instance_retire(instance,accepted,status);
    xr_xir_compile_program_drop(program);xr_xir_value_drop(&owned);
    XrCompileResourceStats released={0};
    CHECK(xr_compile_resources_stats(context->resources,&released)==XR_COMPILE_RESOURCE_OK &&
        released.live_bytes==baseline.live_bytes);
    return completed;
}
static void native_invoke_allocation_failures(const XrXirCompileContext *context,
    const XrXirProgramSpec *const *tables) {
    const uint32_t modes[]={5,9,10,11};
    for (uint32_t m=0;m<4;++m) for (uint32_t cancel=0;cancel<3;++cancel) {
        runtime_fail_at=SIZE_MAX;runtime_attempts=0;
        CHECK(native_invoke_allocation_run(context,tables[modes[m]],modes[m],cancel));
        size_t sites=runtime_attempts;CHECK(sites && !runtime_live && !runtime_bytes);
        for (size_t i=0;i<sites;++i) {
            runtime_fail_at=i;runtime_attempts=0;
            CHECK(!native_invoke_allocation_run(context,tables[modes[m]],modes[m],cancel));
            CHECK(runtime_attempts>i && !runtime_live && !runtime_bytes);
        }
        runtime_fail_at=SIZE_MAX;
        printf("Native invoke physical release: mode=%u cancel=%u allocation failure sites=%zu\n",modes[m],cancel,sites);
    }
}

#include "xir_native_inbox_boundaries.inc.c"

int main(void) {
    SourceFixtureOwner compiler = {0}; source_fixture_owner_new(&compiler);
    const XrXirProgramSpec *tables[]={&fixture_calls0_program,&fixture_calls1_program,&fixture_calls2_program,&fixture_calls3_program,&fixture_calls4_program,&fixture_calls5_program,&fixture_calls6_program,&fixture_calls7_program,&fixture_calls8_program,&fixture_calls9_program,&fixture_calls10_program,&fixture_calls11_program,&fixture_calls12_program,&fixture_calls13_program};
    for (uint32_t mode=0;mode<14;++mode) for (uint32_t cancel=0;cancel<2;++cancel) {
        uint32_t kind=mode>=12?mode-12:mode>=10?mode-10:mode%4;
        XrXirProgram *program=NULL;CHECK(native_program_new(&compiler.context,tables[mode],&program));
        XrXirInstance *instance=call_instance_new(program);
        xr_xir_compile_program_drop(program);program=NULL;
        XrXirDomain *domain=instance->domain;
        XrXirValue arguments[]={{XR_XIR_I64,0,9},{XR_XIR_I64,0,4}};
        CHECK(xr_xir_instance_start(instance,0,arguments,2)==XR_XIR_CALL_READY);
        XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
        if (kind!=2) {
            CHECK(polled.outcome.status==XR_XIR_CALL_SUSPENDED &&
                call_instance_language_depth(instance)==(mode>=12?2u:3u));
            XrXirInstanceResult repeated=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
            CHECK(repeated.epoch==polled.epoch && repeated.outcome.wake==polled.outcome.wake);
            if (cancel) {
                CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
                CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_CANCELLED);
                CHECK(xr_xir_instance_resume(instance,polled.epoch,polled.outcome.wake)==XR_XIR_CALL_BAD_STATE);
            } else CHECK(xr_xir_instance_resume(instance,polled.epoch,polled.outcome.wake)==XR_XIR_CALL_READY);
            polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
        }
        if (mode==9 && !cancel) {
            CHECK(polled.outcome.status==XR_XIR_CALL_SUSPENDED && call_instance_language_depth(instance)==1);
            CHECK(xr_xir_instance_resume(instance,polled.epoch,polled.outcome.wake)==XR_XIR_CALL_READY);
            polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
        }
        XrXirCallResult result=polled.outcome;
        if (kind==2) CHECK(result.status==XR_XIR_CALL_DIVIDE_BY_ZERO && result.value.type==XR_XIR_UNIT);
        else if (cancel) CHECK(result.status==XR_XIR_CALL_CANCELLED && result.value.type==XR_XIR_UNIT);
        else if (kind==3) CHECK(result.status==XR_XIR_CALL_MATCH_FAILURE && result.value.type==XR_XIR_UNIT && xr_xir_fault_match_valid(result.panic.detail));
        else if (mode==1 || mode==9) CHECK(result.status==XR_XIR_CALL_THROWN && error_fixture_is_code(&result.value,domain,91));
        else CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.type==XR_XIR_I64 && result.value.payload==(mode==5 || mode==11 || mode==13?77:mode==10?44:4));
        XrXirValue escaped={0};
        if ((mode==1 || mode==9) && !cancel) CHECK(xr_xir_instance_take_result(instance,&escaped)==XR_XIR_CALL_THROWN);
        call_instance_free(instance,XR_XIR_CALL_READY);
        if (escaped.type) {
            XrCompileResourceStats pinned={0};
            CHECK(xr_compile_resources_stats(compiler.context.resources,&pinned)==XR_COMPILE_RESOURCE_OK);
            CHECK(pinned.live_bytes>compiler.baseline.live_bytes);
            XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
            CHECK(error_fixture_is_code(&escaped,reader,91));
            xr_xir_value_drop(&escaped);xr_xir_domain_drop(reader);
        }
        XrCompileResourceStats released={0};
        CHECK(xr_compile_resources_stats(compiler.context.resources,&released)==XR_COMPILE_RESOURCE_OK);
        CHECK(released.live_bytes==compiler.baseline.live_bytes);
    }
    CHECK(!runtime_live && !runtime_bytes);
    native_invoke_allocation_failures(&compiler.context, tables);
    native_inbox_boundaries(&compiler.context);
    source_fixture_owner_free(&compiler);
    puts("Native resumable calls, error propagation, cancellation and physical release passed");
    return 0;
}
