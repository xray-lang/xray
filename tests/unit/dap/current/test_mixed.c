/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mixed.c - Real emitted native boundaries, VM values and child identities
 */
#include "app/dap/xdap_current.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "../../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"mixed debug FAIL %d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../../xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
XR_DATA const XrXirProgramSpec child_callable_native_program;
enum { MIX_INIT,MIX_ENTRY,MIX_ROOT,MIX_CHILD,MIX_PURE,MIX_RELAY,MIX_ROOT_LEAF,MIX_UNKNOWN,MIX_BOTH,MIX_FACTORY,MIX_FUNCTIONS };
typedef struct MixedFixture {
    XrCompileResources *resources;
    XrCompileResourceStats baseline;
    XrXirCompileContext context;
    XrXirArtifact *lowered;
    XrXirDomain *domain;
    XrXirVmDebug *debug;
    XrXirProgram *program;
    XdapCurrent *session;
    unsigned releases,outputs;
    bool root_native;
} MixedFixture;
static void mixed_code_release(void *owner) { ++((MixedFixture *)owner)->releases; }
static XrXirOutputStatus mixed_output(void *owner,const XrXirOutputGroup *group) {
    MixedFixture *fixture=owner;CHECK(group && group->stream==XR_XIR_STDOUT && group->line && group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64 && group->values[0].payload==42);++fixture->outputs;return XR_XIR_OUTPUT_OK;
}
static void mixed_prepare(MixedFixture *fixture) {
    memset(fixture,0,sizeof(*fixture));
    XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture->resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture->resources,&fixture->baseline)==XR_COMPILE_RESOURCE_OK);
    fixture->context=(XrXirCompileContext){fixture->resources,xr_xir_compile_default_limits()};
    XrXirProgramSpec native=child_callable_native_program;CHECK(native.entry_count==MIX_FUNCTIONS);
    XrXirArtifact *checked=NULL,*closed=NULL;
    CHECK(xr_xir_compile_checked_read(&fixture->context,native.proof.bytes,native.proof.length,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_lower(closed,&native.target,&fixture->lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture->domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_vm_debug_new(fixture->domain,64,&fixture->debug)==XR_XIR_VALUE_OK);
}
static void mixed_new(MixedFixture *fixture,bool root_native) {
    mixed_prepare(fixture);fixture->root_native=root_native;
    XrXirProgramSpec native=child_callable_native_program;
    unsigned char mask[MIX_FUNCTIONS];memset(mask,root_native ? 1 : 0,sizeof(mask));
    mask[MIX_CHILD]=mask[MIX_PURE]=root_native ? 0 : 1;
    native.code=(XrXirCodeLease){fixture,mixed_code_release};
    CHECK(xr_xir_compile_vm_debug_mixed_program_take(&fixture->lowered,&native,mask,MIX_FUNCTIONS,
        fixture->debug,&fixture->program)==XR_XIR_OK && !fixture->lowered && !native.code.owner && !native.code.release);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=config.value_limit=config.call_limit=1048576;
    config.requested_value_limit=config.requested_call_limit=1048576;
    config.work_limit=128000000;config.poll_limit=10000;config.depth_limit=64;
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,mixed_output,fixture};
    CHECK(xdap_current_new(fixture->domain,&fixture->debug,fixture->program,&config,&fixture->session)==XR_XIR_VALUE_OK);
}
static void mixed_constructor_close(MixedFixture *fixture) {
    xr_xir_compile_program_drop(fixture->program);xr_xir_compile_artifact_free(fixture->lowered);
    xr_xir_vm_debug_drop(fixture->debug);
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static void mixed_rejections(void) {
    MixedFixture fixture;mixed_prepare(&fixture);
    XrXirProgramSpec native=child_callable_native_program;native.code=(XrXirCodeLease){&fixture,mixed_code_release};
    unsigned char mask[MIX_FUNCTIONS]={0};mask[MIX_CHILD]=mask[MIX_PURE]=1;
    XrXirArtifact *artifact=fixture.lowered;XrXirProgramSpec before=native;
    CHECK(xr_xir_compile_vm_debug_mixed_program_take(&fixture.lowered,&native,mask,MIX_FUNCTIONS-1,
        fixture.debug,&fixture.program)==XR_XIR_BAD_STRUCTURE);
    CHECK(fixture.lowered==artifact && !fixture.program && !memcmp(&native,&before,sizeof(native)) && !fixture.releases);
    unsigned char identity[32];memcpy(identity,native.proof.identity,sizeof(identity));identity[0]^=1;
    native.proof.identity=identity;
    CHECK(xr_xir_compile_vm_debug_mixed_program_take(&fixture.lowered,&native,mask,MIX_FUNCTIONS,
        fixture.debug,&fixture.program)==XR_XIR_BAD_STRUCTURE);
    CHECK(fixture.lowered==artifact && !fixture.program && !fixture.releases);native=before;
    mask[0]=2;
    CHECK(xr_xir_compile_vm_debug_mixed_program_take(&fixture.lowered,&native,mask,MIX_FUNCTIONS,
        fixture.debug,&fixture.program)==XR_XIR_BAD_STRUCTURE);mask[0]=0;
    memset(mask,1,sizeof(mask));
    CHECK(xr_xir_compile_vm_debug_mixed_program_take(&fixture.lowered,&native,mask,MIX_FUNCTIONS,
        fixture.debug,&fixture.program)==XR_XIR_UNSUPPORTED);
    CHECK(fixture.lowered==artifact && !fixture.program && !memcmp(&native,&before,sizeof(native)) && !fixture.releases);
    mixed_constructor_close(&fixture);
}
static size_t mixed_constructor_fault(size_t fail,bool root_native) {
    MixedFixture fixture;mixed_prepare(&fixture);
    XrXirProgramSpec native=child_callable_native_program;native.code=(XrXirCodeLease){&fixture,mixed_code_release};
    XrXirProgramSpec before=native;XrXirArtifact *artifact=fixture.lowered;
    unsigned char mask[MIX_FUNCTIONS];memset(mask,root_native ? 1 : 0,sizeof(mask));mask[MIX_CHILD]=mask[MIX_PURE]=root_native ? 0 : 1;
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(fixture.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xr_xir_compile_vm_debug_mixed_program_take(&fixture.lowered,&native,mask,MIX_FUNCTIONS,fixture.debug,&fixture.program);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    CHECK(status==(fail==SIZE_MAX ? XR_XIR_OK : XR_XIR_OUT_OF_MEMORY));
    if (fail!=SIZE_MAX) {
        CHECK(sites>fail && fixture.lowered==artifact && !fixture.program && !memcmp(&native,&before,sizeof(native)) && !fixture.releases);
        XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(fixture.resources,&spent)==XR_COMPILE_RESOURCE_OK);
        CHECK(spent.live_bytes==baseline.live_bytes);
        CHECK(xr_xir_compile_vm_debug_mixed_program_take(&fixture.lowered,&native,mask,MIX_FUNCTIONS,
            fixture.debug,&fixture.program)==XR_XIR_OK);
        XrCompileResourceStats retry={0};CHECK(xr_compile_resources_stats(fixture.resources,&retry)==XR_COMPILE_RESOURCE_OK);
        CHECK(retry.allocated_bytes>=spent.allocated_bytes && retry.work>=spent.work);
    }
    CHECK(!fixture.lowered && !native.code.owner && !native.code.release);mixed_constructor_close(&fixture);CHECK(fixture.releases==1);return sites;
}
static XdapCurrentResult mixed_poll(MixedFixture *fixture) {
    XdapCurrentResult result={0};CHECK(xdap_current_poll(fixture->session,1000,&result)==XR_XIR_VALUE_OK);return result;
}
static XrXirVmDebugFrameInfo mixed_frame(XrXirVmDebugSnapshot *snapshot,uint32_t index,const char *expected) {
    XrXirVmDebugFrameInfo info={0};XrXirValue name={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,index,&info,&name)==XR_XIR_VALUE_OK);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&name,&bytes,&length));
    CHECK(length==strlen(expected) && !memcmp(bytes,expected,length));xr_xir_value_drop(&name);return info;
}
static void mixed_run(bool root_native,bool cancel) {
    MixedFixture fixture;mixed_new(&fixture,root_native);
    uint64_t threads[2]={0};uint32_t thread_count=77;
    CHECK(xdap_current_threads(fixture.session,threads,2,&thread_count)==XR_XIR_VALUE_OK && !thread_count);
    uint32_t stop=root_native ? MIX_PURE : MIX_RELAY;
    CHECK(xdap_current_breakpoint(fixture.session,stop,0,true)==XR_XIR_VALUE_OK);
    CHECK(xdap_current_start(fixture.session,MIX_ROOT,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xdap_current_threads(fixture.session,threads,2,&thread_count)==XR_XIR_VALUE_OK && thread_count==1 && threads[0]==1);
    if (root_native) {
        XdapCurrentResult ingress={0};CHECK(xdap_current_poll(fixture.session,1,&ingress)==XR_XIR_VALUE_OK);
        CHECK(ingress.state==XDAP_CURRENT_RUNNING);xdap_current_result_drop(&ingress);
        threads[0]=123;thread_count=77;
        CHECK(xdap_current_threads(fixture.session,threads,2,&thread_count)==XR_XIR_VALUE_UNSUPPORTED);
        CHECK(threads[0]==123 && thread_count==77);
    }
    xr_xir_compile_program_drop(fixture.program);fixture.program=NULL;
    XdapCurrentResult result=mixed_poll(&fixture);CHECK(result.state==XDAP_CURRENT_STOPPED);
    XrXirVmDebugSnapshot *escaped=NULL;CHECK(xdap_current_snapshot(fixture.session,&escaped)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_vm_debug_snapshot_depth(escaped)==(root_native ? 4u : 3u));
    XrXirVmDebugFrameInfo top=mixed_frame(escaped,0,root_native ? "pure" : "relay");
    XrXirVmDebugFrameInfo native=mixed_frame(escaped,1,root_native ? "relay" : "child");
    CHECK(top.code==XR_XIR_VM_DEBUG_VM && top.thread>1 && top.thread==native.thread && top.value_count);
    CHECK(native.code==XR_XIR_VM_DEBUG_NATIVE && !native.value_count && native.instruction==UINT32_MAX);
    CHECK(xr_xir_vm_debug_snapshot_stopped_thread(escaped)==top.thread);
    CHECK(xdap_current_threads(fixture.session,threads,2,&thread_count)==XR_XIR_VALUE_OK && thread_count==2);
    CHECK(threads[0]==1 && threads[1]==top.thread);
    XrXirVmDebugFrameInfo root=mixed_frame(escaped,root_native ? 3u : 2u,"root");
    CHECK(root.thread==1 && !root.value_count && root.code==(root_native ? XR_XIR_VM_DEBUG_NATIVE : XR_XIR_VM_DEBUG_VM));
    XrXirValue absent={0};CHECK(xr_xir_vm_debug_snapshot_value(escaped,1,0,&absent)==XR_XIR_VALUE_BAD_ARGUMENT);
    if (cancel) {
        CHECK(xdap_current_stop(fixture.session)==XR_XIR_CALL_READY);xdap_current_result_drop(&result);
        result=mixed_poll(&fixture);CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_CANCELLED);
        CHECK(!fixture.outputs);
    } else {
        CHECK(xdap_current_breakpoint(fixture.session,stop,0,false)==XR_XIR_VALUE_OK);
        CHECK(xdap_current_continue(fixture.session,result.debug_epoch,XR_XIR_VM_DEBUG_STEP_OUT)==XR_XIR_VALUE_OK);
        CHECK(xdap_current_threads(fixture.session,threads,2,&thread_count)==XR_XIR_VALUE_OK && thread_count==2);
        CHECK(threads[0]==1 && threads[1]==top.thread);
        xdap_current_result_drop(&result);result=mixed_poll(&fixture);
        if (root_native) {
            CHECK(result.state==XDAP_CURRENT_STOPPED);XrXirVmDebugSnapshot *next=NULL;
            CHECK(xdap_current_snapshot(fixture.session,&next)==XR_XIR_VALUE_OK);
            XrXirVmDebugFrameInfo caller=mixed_frame(next,0,"child");CHECK(caller.thread==top.thread && caller.code==XR_XIR_VM_DEBUG_VM);
            CHECK(caller.id!=top.id && xr_xir_vm_debug_snapshot_depth(next)==2);
            xr_xir_vm_debug_snapshot_drop(next);
            CHECK(xdap_current_continue(fixture.session,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
            xdap_current_result_drop(&result);result=mixed_poll(&fixture);
        }
        CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(result.outcome.value.type==XR_XIR_I64 && result.outcome.value.payload==42 && fixture.outputs==1);
    }
    CHECK(xdap_current_threads(fixture.session,threads,2,&thread_count)==XR_XIR_VALUE_OK && !thread_count);
    xdap_current_result_drop(&result);XrXirCallStatus retired=xdap_current_free(fixture.session);
    CHECK(retired==XR_XIR_CALL_READY);fixture.session=NULL;
    XrXirVmDebugFrameInfo still=mixed_frame(escaped,1,root_native ? "relay" : "child");CHECK(still.id==native.id && still.thread==native.thread);
    xr_xir_vm_debug_snapshot_drop(escaped);CHECK(fixture.releases==1);
    CHECK(!xr_xir_domain_budget_stats(fixture.domain).metadata_live);xr_xir_domain_drop(fixture.domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture.baseline.live_bytes);xr_compile_resources_release(fixture.resources);
    CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();
    if (argc==2 && !strcmp(argv[1],"--resources")) {
        for (unsigned native=0;native<2;++native) {
            size_t sites=mixed_constructor_fault(SIZE_MAX,native!=0);CHECK(sites);
            for (size_t i=0;i<sites;++i) mixed_constructor_fault(i,native!=0);
            printf("Mixed debug constructor direction=%u: %zu allocation sites, owned input retry and physical=0/0 PASS\n",native,sites);
        }
        return 0;
    }
    CHECK(argc==1);mixed_rejections();mixed_run(false,false);mixed_run(true,false);mixed_run(false,true);mixed_run(true,true);
    puts("Authentic VM/native/VM and native/VM/native metadata, child identity, step boundaries, cancel and escaped owners: PASS");return 0;
}
