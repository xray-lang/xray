/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_current_debug.c - Real Source/Program debug control and owned observations
 */
#include "app/dap/xdap_current.h"
#include "xir/xxir_source.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_value_internal.h"
#include "toolchain/xcompiler_session.h"
#include "../../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "../../xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"

typedef struct Fixture {
    XrXirCompileContext context;
    XrCompileResourceStats baseline;
    XrXirProgram *program;
    XrXirDomain *metadata;
    uint64_t metadata_baseline;
    XrXirVmDebug *debug;
    XdapCurrent *session;
    uint32_t probe, leaf, return_at, string_id, call_at, observed_id;
} Fixture;
static const char source_text[] =
    "fn leaf(value:i64)->string { var observed=value*2; const text=\"owned\\0value\"; "
    "observed=observed+1; return text }\n"
    "export fn probe(value:i64)->string { return leaf(value+1) }\n";
static XrXirArtifact *fixture_prepare(Fixture *fixture) {
    memset(fixture,0,sizeof(*fixture));
    XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture->context.resources)==XR_COMPILE_RESOURCE_OK);
    fixture->context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_stats(fixture->context.resources,&fixture->baseline)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *producer=NULL;
    CHECK(xr_compile_session_new(fixture->context.resources,&producer)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"dap-debug",NULL};
    XrXirSourceRequest request={.session=producer,.authority=&authority,
        .context=&fixture->context,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText text={NULL,source_text,sizeof(source_text)-1};
    XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,&result,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"source %u: %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK);
    xr_compile_session_free(producer);
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(result.checked,&closed,NULL, NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    xr_xir_compile_source_result_free(&result);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    fixture->leaf=fixture->probe=UINT32_MAX;
    for (uint32_t i=0;i<module->function_count;++i) {
        const XrXirFunction *function=&module->functions[i];
        if (function->name_length==4 && !memcmp(function->name,"leaf",4)) fixture->leaf=i;
        if (function->name_length==5 && !memcmp(function->name,"probe",5)) fixture->probe=i;
    }
    CHECK(fixture->leaf!=UINT32_MAX && fixture->probe!=UINT32_MAX);
    const XrXirFunction *leaf=&module->functions[fixture->leaf];
    fixture->return_at=fixture->string_id=fixture->observed_id=UINT32_MAX;
    for (uint32_t i=0;i<leaf->instruction_count;++i) {
        if (leaf->instructions[i].op==XR_XIR_RETURN) fixture->return_at=i;
        if (leaf->instructions[i].op==XR_XIR_SCALAR_LOCAL_WRITE || leaf->instructions[i].op==XR_XIR_CELL_LOCAL_WRITE || leaf->instructions[i].op==XR_XIR_CELL_WRITE) fixture->observed_id=leaf->instructions[i].args[0];
        if (leaf->instructions[i].op==XR_XIR_CONST_STRING) fixture->string_id=leaf->parameter_count+i;
    }
    fixture->call_at=UINT32_MAX;
    const XrXirFunction *probe=&module->functions[fixture->probe];
    for (uint32_t i=0;i<probe->instruction_count;++i)
        if (probe->instructions[i].op==XR_XIR_CALL || probe->instructions[i].op==XR_XIR_INVOKE) fixture->call_at=i;
    CHECK(fixture->call_at!=UINT32_MAX);
    CHECK(fixture->return_at!=UINT32_MAX && fixture->string_id!=UINT32_MAX && fixture->observed_id!=UINT32_MAX);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture->metadata)==XR_XIR_VALUE_OK);
    fixture->metadata_baseline=xr_xir_domain_stats(fixture->metadata).live_bytes;
    return lowered;
}
static XrXirInstanceConfig fixture_config(void) {
    XrXirInstanceConfig config={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=config.value_limit=config.call_limit=1048576;
    config.requested_value_limit=config.requested_call_limit=1048576;
    config.work_limit=128000000; config.poll_limit=10000; config.depth_limit=64;
    return config;
}
static void fixture_new_capacity(Fixture *fixture,uint32_t capacity) {
    XrXirArtifact *lowered=fixture_prepare(fixture);
    CHECK(xr_xir_vm_debug_new(fixture->metadata,capacity,&fixture->debug)==XR_XIR_VALUE_OK);
    XrXirArtifact *original=lowered;
    CHECK(xr_xir_compile_vm_debug_program_take(&lowered,fixture->debug,&fixture->program)==XR_XIR_OK);
    CHECK(!lowered && fixture->program->permissions->input->artifact==original);
    XrXirInstanceConfig config=fixture_config();
    CHECK(xdap_current_new(fixture->metadata,&fixture->debug,fixture->program,&config,&fixture->session)==XR_XIR_VALUE_OK);
    CHECK(!fixture->debug);
    xr_xir_compile_program_drop(fixture->program);fixture->program=NULL;
}
static void fixture_new(Fixture *fixture) { fixture_new_capacity(fixture,64); }
static void fixture_free(Fixture *fixture) {
    XrXirCallStatus freed=xdap_current_free(fixture->session);
    if (freed!=XR_XIR_CALL_READY) fprintf(stderr,"session free status=%u\n",freed);
    CHECK(freed==XR_XIR_CALL_READY);
    fixture->session=NULL;
}
static void fixture_finish(Fixture *fixture) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(fixture->context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes==fixture->baseline.live_bytes);
    xr_compile_resources_release(fixture->context.resources);
    fixture->context=(XrXirCompileContext){0};
}
static XdapCurrentResult poll_fixture(Fixture *fixture) {
    XdapCurrentResult result={0};
    CHECK(xdap_current_poll(fixture->session,1000,&result)==XR_XIR_VALUE_OK);
    return result;
}
static void start(Fixture *fixture) {
    XrXirValue argument={XR_XIR_I64,0,19};
    CHECK(xdap_current_start(fixture->session,fixture->probe,&argument,1)==XR_XIR_CALL_READY);
}
static void assert_string(const XrXirValue *value) {
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length));
    CHECK(length==11 && !memcmp(bytes,"owned\0value",11));
}
static uint64_t assert_frame(XrXirVmDebugSnapshot *snapshot,uint32_t at,uint32_t function,int64_t parameter) {
    XrXirVmDebugFrameInfo info={0};XrXirValue name={0},value={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,at,&info,&name)==XR_XIR_VALUE_OK);
    CHECK(info.function==function && info.id && info.epoch);
    CHECK(xr_xir_vm_debug_snapshot_value(snapshot,at,0,&value)==XR_XIR_VALUE_OK);
    CHECK(value.type==XR_XIR_I64 && value.payload==parameter);
    xr_xir_value_drop(&name);xr_xir_value_drop(&value);
    return info.epoch;
}
static void breakpoint_and_escape(void) {
    Fixture fixture;fixture_new(&fixture);
    XrXirVmDebugLocation installed={fixture.leaf,fixture.return_at};
    CHECK(xdap_current_breakpoints_replace(fixture.session,&installed,1)==XR_XIR_VALUE_OK);
    XrXirVmDebugLocation rejected[2]={{fixture.leaf,0},{UINT32_MAX,0}};
    CHECK(xdap_current_breakpoints_replace(fixture.session,rejected,2)==XR_XIR_VALUE_BAD_ARGUMENT);
    start(&fixture);
    XdapCurrentResult result=poll_fixture(&fixture);
    CHECK(result.state==XDAP_CURRENT_STOPPED && result.debug_epoch);
    uint64_t epoch=result.debug_epoch;
    XrXirVmDebugSnapshot *snapshot=NULL;
    CHECK(xdap_current_snapshot(fixture.session,&snapshot)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_vm_debug_snapshot_depth(snapshot)==2);
    CHECK(assert_frame(snapshot,0,fixture.leaf,20)==epoch);
    CHECK(assert_frame(snapshot,1,fixture.probe,19)==epoch);
    XrXirValue value={0};
    CHECK(xr_xir_vm_debug_snapshot_value(snapshot,0,fixture.string_id,&value)==XR_XIR_VALUE_OK);
    assert_string(&value);xr_xir_value_drop(&value);
    CHECK(xr_xir_vm_debug_snapshot_cell_value(snapshot,0,fixture.observed_id,&value)==XR_XIR_VALUE_OK);
    CHECK(value.type==XR_XIR_I64 && value.payload==41);xr_xir_value_drop(&value);
    CHECK(xr_xir_vm_debug_snapshot_value(snapshot,0,UINT32_MAX,&value)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!value.type && !value.payload);
    CHECK(xdap_current_continue(fixture.session,epoch+1,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_BAD_ARGUMENT);
    xdap_current_result_drop(&result);
    result=poll_fixture(&fixture);CHECK(result.state==XDAP_CURRENT_STOPPED && result.debug_epoch==epoch);
    xdap_current_result_drop(&result);
    CHECK(xdap_current_continue(fixture.session,epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
    result=poll_fixture(&fixture);CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_RETURNED);
    assert_string(&result.outcome.value);xdap_current_result_drop(&result);
    fixture_free(&fixture);
    CHECK(xr_xir_vm_debug_snapshot_value(snapshot,0,fixture.string_id,&value)==XR_XIR_VALUE_OK);
    assert_string(&value);xr_xir_value_drop(&value);
    CHECK(xr_xir_vm_debug_snapshot_cell_value(snapshot,0,fixture.observed_id,&value)==XR_XIR_VALUE_OK);
    CHECK(value.type==XR_XIR_I64 && value.payload==41);xr_xir_value_drop(&value);
    xr_xir_vm_debug_snapshot_drop(snapshot);
    fixture_finish(&fixture);
    XrXirDomainBudgetStats budget=xr_xir_domain_budget_stats(fixture.metadata);
    CHECK(!budget.metadata_live && !budget.call_live && budget.metadata_allocations==budget.metadata_frees);
    CHECK(xr_xir_domain_stats(fixture.metadata).live_bytes==fixture.metadata_baseline);
    xr_xir_domain_drop(fixture.metadata);
    CHECK(!runtime_live && !runtime_bytes);
}
static void stepping_and_cancel(void) {
    Fixture fixture;fixture_new(&fixture);
    CHECK(xdap_current_breakpoint(fixture.session,fixture.leaf,0,true)==XR_XIR_VALUE_OK);
    start(&fixture);XdapCurrentResult result=poll_fixture(&fixture);
    CHECK(result.state==XDAP_CURRENT_STOPPED);
    XrXirVmDebugSnapshot *first=NULL;CHECK(xdap_current_snapshot(fixture.session,&first)==XR_XIR_VALUE_OK);
    CHECK(assert_frame(first,0,fixture.leaf,20)==result.debug_epoch);
    XrXirValue untouched={0};
    CHECK(xr_xir_vm_debug_snapshot_value(first,0,1,&untouched)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xdap_current_continue(fixture.session,result.debug_epoch,XR_XIR_VM_DEBUG_STEP_IN)==XR_XIR_VALUE_OK);
    xdap_current_result_drop(&result);result=poll_fixture(&fixture);CHECK(result.state==XDAP_CURRENT_STOPPED);
    XrXirVmDebugSnapshot *second=NULL;CHECK(xdap_current_snapshot(fixture.session,&second)==XR_XIR_VALUE_OK);
    XrXirVmDebugFrameInfo info={0};XrXirValue name={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(second,0,&info,&name)==XR_XIR_VALUE_OK);
    CHECK(info.instruction==1);xr_xir_value_drop(&name);
    CHECK(xdap_current_continue(fixture.session,result.debug_epoch,XR_XIR_VM_DEBUG_STEP_OUT)==XR_XIR_VALUE_OK);
    xdap_current_result_drop(&result);result=poll_fixture(&fixture);CHECK(result.state==XDAP_CURRENT_STOPPED);
    XrXirVmDebugSnapshot *caller=NULL;CHECK(xdap_current_snapshot(fixture.session,&caller)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_vm_debug_snapshot_depth(caller)==1);
    CHECK(assert_frame(caller,0,fixture.probe,19)==result.debug_epoch);
    CHECK(xdap_current_cancel(fixture.session)==XR_XIR_CALL_CANCEL_REQUESTED);
    xdap_current_result_drop(&result);result=poll_fixture(&fixture);CHECK(result.outcome.status==XR_XIR_CALL_CANCELLED);
    xdap_current_result_drop(&result);fixture_free(&fixture);
    xr_xir_vm_debug_snapshot_drop(first);xr_xir_vm_debug_snapshot_drop(second);xr_xir_vm_debug_snapshot_drop(caller);
    fixture_finish(&fixture);
    CHECK(!xr_xir_domain_budget_stats(fixture.metadata).metadata_live);
    xr_xir_domain_drop(fixture.metadata);CHECK(!runtime_live && !runtime_bytes);
}

static void fixture_dispose(Fixture *fixture, XrXirArtifact *lowered) {
    if (fixture->session) fixture_free(fixture);
    xr_xir_compile_program_drop(fixture->program);
    xr_xir_compile_artifact_free(lowered);
    xr_xir_vm_debug_drop(fixture->debug);
    fixture_finish(fixture);
    XrXirDomainBudgetStats budget=xr_xir_domain_budget_stats(fixture->metadata);
    CHECK(!budget.metadata_live && !budget.call_live);
    CHECK(budget.metadata_allocations==budget.metadata_frees);
    CHECK(xr_xir_domain_stats(fixture->metadata).live_bytes==fixture->metadata_baseline);
    xr_xir_domain_drop(fixture->metadata);
    CHECK(!runtime_live && !runtime_bytes);
}
static size_t failure_slice(unsigned stage,size_t failure) {
    Fixture fixture;XrXirArtifact *lowered=fixture_prepare(&fixture);
    XrXirInstanceConfig config=fixture_config();
    if (stage>0 && stage<4) CHECK(xr_xir_vm_debug_new(fixture.metadata,64,&fixture.debug)==XR_XIR_VALUE_OK);
    if (stage>1 && stage<4) CHECK(xr_xir_compile_vm_debug_program_take(&lowered,fixture.debug,&fixture.program)==XR_XIR_OK);
    if (stage==3) {
        CHECK(xdap_current_new(fixture.metadata,&fixture.debug,fixture.program,&config,&fixture.session)==XR_XIR_VALUE_OK);
        CHECK(xdap_current_breakpoint(fixture.session,fixture.leaf,fixture.return_at,true)==XR_XIR_VALUE_OK);
    }
    size_t before=runtime_attempts;
    runtime_fail_at=failure==SIZE_MAX ? SIZE_MAX : before+failure;
    XrXirDomainBudgetStats budget_before=xr_xir_domain_budget_stats(fixture.metadata);
    XrCompileResourceStats compiler_before={0};
    CHECK(xr_compile_resources_stats(fixture.context.resources,&compiler_before)==XR_COMPILE_RESOURCE_OK);
    if (!stage) {
        XrXirValueStatus status=xr_xir_vm_debug_new(fixture.metadata,64,&fixture.debug);
        CHECK(status==(failure==SIZE_MAX ? XR_XIR_VALUE_OK : XR_XIR_VALUE_OOM));
        if (failure!=SIZE_MAX) CHECK(!fixture.debug);
    } else if (stage==1 || stage==4) {
        XrXirArtifact *input=lowered;
        XrXirStatus status=stage==4 ? xr_xir_compile_vm_program_take(&lowered,&fixture.program) :
            xr_xir_compile_vm_debug_program_take(&lowered,fixture.debug,&fixture.program);
        CHECK(status==(failure==SIZE_MAX ? XR_XIR_OK : XR_XIR_OUT_OF_MEMORY));
        if (failure!=SIZE_MAX) {
            CHECK(lowered==input && !fixture.program);
            XrCompileResourceStats after={0};
            CHECK(xr_compile_resources_stats(fixture.context.resources,&after)==XR_COMPILE_RESOURCE_OK);
            CHECK(after.live_bytes==compiler_before.live_bytes && after.work>compiler_before.work);
        }
    } else if (stage==2) {
        XrXirVmDebug *input=fixture.debug;
        XrXirValueStatus status=xdap_current_new(fixture.metadata,&fixture.debug,fixture.program,&config,&fixture.session);
        CHECK(status==(failure==SIZE_MAX ? XR_XIR_VALUE_OK : XR_XIR_VALUE_OOM));
        if (failure!=SIZE_MAX) CHECK(fixture.debug==input && !fixture.session);
    } else {
        XrXirValue argument={XR_XIR_I64,0,19};
        XrXirCallStatus started=xdap_current_start(fixture.session,fixture.probe,&argument,1);
        if (started==XR_XIR_CALL_READY) {
            XdapCurrentResult result=poll_fixture(&fixture);
            if (failure==SIZE_MAX) CHECK(result.state==XDAP_CURRENT_STOPPED);
            else CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_OOM);
            xdap_current_result_drop(&result);
        } else CHECK(failure!=SIZE_MAX && started==XR_XIR_CALL_OOM);
    }
    size_t attempts=runtime_attempts-before;
    CHECK(failure==SIZE_MAX || attempts>failure);
    runtime_fail_at=SIZE_MAX;
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(fixture.metadata);
    CHECK(after.requested_call_bytes>=budget_before.requested_call_bytes && after.work>=budget_before.work);
    if (failure!=SIZE_MAX && stage!=3) {
        CHECK(after.metadata_live==budget_before.metadata_live);
        /* Retry on the same cumulative owner after the failed allocation. */
        if (!stage) CHECK(xr_xir_vm_debug_new(fixture.metadata,64,&fixture.debug)==XR_XIR_VALUE_OK);
        else if (stage==4) CHECK(xr_xir_compile_vm_program_take(&lowered,&fixture.program)==XR_XIR_OK);
        else if (stage==1) CHECK(xr_xir_compile_vm_debug_program_take(&lowered,fixture.debug,&fixture.program)==XR_XIR_OK);
        else CHECK(xdap_current_new(fixture.metadata,&fixture.debug,fixture.program,&config,&fixture.session)==XR_XIR_VALUE_OK);
        XrXirDomainBudgetStats retry=xr_xir_domain_budget_stats(fixture.metadata);
        CHECK(retry.requested_call_bytes>=after.requested_call_bytes && retry.work>=after.work);
    }
    if (stage==3 && fixture.session) {
        XrXirCallStatus disposed=xdap_current_free(fixture.session);
        CHECK(disposed==XR_XIR_CALL_READY || (failure!=SIZE_MAX && disposed==XR_XIR_CALL_OOM));
        fixture.session=NULL;
    }
    fixture_dispose(&fixture,lowered);
    return attempts;
}
static void allocation_failures(void) {
    for (unsigned stage=0;stage<5;++stage) {
        size_t sites=failure_slice(stage,SIZE_MAX);CHECK(sites);
        for (size_t i=0;i<sites;++i) {
            fprintf(stderr,"Fault slice %u allocation %zu/%zu\n",stage,i+1,sites);
            failure_slice(stage,i);
        }
        printf("Fault slice %u: %zu allocation sites PASS\n",stage,sites);
    }
}
static void pause_and_step_over(void) {
    Fixture fixture;fixture_new(&fixture);
    start(&fixture);xdap_current_pause(fixture.session);
    XdapCurrentResult result=poll_fixture(&fixture);CHECK(result.state==XDAP_CURRENT_STOPPED);
    XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(fixture.session,&snapshot)==XR_XIR_VALUE_OK);
    XrXirVmDebugFrameInfo info={0};XrXirValue name={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,0,&info,&name)==XR_XIR_VALUE_OK);
    CHECK(info.instruction==0);xr_xir_value_drop(&name);
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(fixture.metadata);
    for (unsigned i=0;i<10;++i) {
        XdapCurrentResult again=poll_fixture(&fixture);CHECK(again.debug_epoch==result.debug_epoch);
        xdap_current_result_drop(&again);
    }
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(fixture.metadata);
    CHECK(before.work==after.work && before.requested_call_bytes==after.requested_call_bytes);
    CHECK(xdap_current_continue(fixture.session,result.debug_epoch,(XrXirVmDebugMode)-1)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xdap_current_continue(fixture.session,result.debug_epoch,XR_XIR_VM_DEBUG_LINE_OVER)==XR_XIR_VALUE_UNSUPPORTED);
    XrXirVmDebugSnapshot *unchanged=NULL;
    CHECK(xdap_current_snapshot(fixture.session,&unchanged)==XR_XIR_VALUE_OK && unchanged==snapshot);
    xr_xir_vm_debug_snapshot_drop(unchanged);
    CHECK(xdap_current_breakpoint(fixture.session,fixture.probe,fixture.call_at,true)==XR_XIR_VALUE_OK);
    CHECK(xdap_current_continue(fixture.session,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
    xdap_current_result_drop(&result);result=poll_fixture(&fixture);CHECK(result.state==XDAP_CURRENT_STOPPED);
    CHECK(xdap_current_continue(fixture.session,result.debug_epoch,XR_XIR_VM_DEBUG_STEP_OVER)==XR_XIR_VALUE_OK);
    xdap_current_result_drop(&result);result=poll_fixture(&fixture);CHECK(result.state==XDAP_CURRENT_STOPPED);
    XrXirVmDebugSnapshot *next=NULL;CHECK(xdap_current_snapshot(fixture.session,&next)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_vm_debug_snapshot_depth(next)==1);
    CHECK(xr_xir_vm_debug_snapshot_frame(next,0,&info,&name)==XR_XIR_VALUE_OK);
    CHECK(info.function==fixture.probe && info.instruction==fixture.call_at+1);xr_xir_value_drop(&name);
    CHECK(xdap_current_cancel(fixture.session)==XR_XIR_CALL_CANCEL_REQUESTED);
    xdap_current_result_drop(&result);fixture_free(&fixture);
    xr_xir_vm_debug_snapshot_drop(snapshot);xr_xir_vm_debug_snapshot_drop(next);
    fixture_dispose(&fixture,NULL);
}


static void admission_rejections(void) {
    Fixture fixture;XrXirArtifact *lowered=fixture_prepare(&fixture);
    XrXirInstanceConfig config=fixture_config();
    union { uint64_t prefix; XrXirVmDebug *alignment; } retired={0};
    CHECK(!xr_xir_vm_debug_paused((XrXirVmDebug *)&retired));
    xr_xir_vm_debug_request_pause((XrXirVmDebug *)&retired);CHECK(!retired.prefix);
    CHECK(xr_xir_vm_debug_snapshot_depth((XrXirVmDebugSnapshot *)&retired)==0);
    CHECK(xr_xir_vm_debug_new(fixture.metadata,0,&fixture.debug)==XR_XIR_VALUE_BAD_ARGUMENT);
    uint32_t references=atomic_load(&fixture.metadata->references);
    atomic_store(&fixture.metadata->references,UINT32_MAX);
    CHECK(xr_xir_vm_debug_new(fixture.metadata,64,&fixture.debug)==XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(!fixture.debug);atomic_store(&fixture.metadata->references,references);
    CHECK(xr_xir_vm_debug_new(fixture.metadata,64,&fixture.debug)==XR_XIR_VALUE_OK);
    XrXirArtifact *input=lowered;
    xr_xir_vm_debug_disable(fixture.debug);
    CHECK(xr_xir_compile_vm_debug_program_take(&lowered,fixture.debug,&fixture.program)==XR_XIR_BAD_STRUCTURE);
    CHECK(lowered==input && !fixture.program);
    xr_xir_vm_debug_drop(fixture.debug);fixture.debug=NULL;
    CHECK(xr_xir_vm_debug_new(fixture.metadata,64,&fixture.debug)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_vm_debug_program_take(&lowered,fixture.debug,&fixture.program)==XR_XIR_OK);
    CHECK(xr_xir_vm_debug_breakpoint(fixture.debug,UINT32_MAX,0,true)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_vm_debug_breakpoint(fixture.debug,fixture.leaf,UINT32_MAX,true)==XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirInstance *foreign=NULL;
    CHECK(xr_xir_vm_debug_instance_new(fixture.debug,(XrXirProgram *)(uintptr_t)1,&config,&foreign)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!foreign);
    /* An ordinary Instance cannot drive the observer's authenticated target. */
    CHECK(xr_xir_instance_new(fixture.program,&config,&foreign)==XR_XIR_CALL_READY);
    XrXirValue argument={XR_XIR_I64,0,19};
    CHECK(xr_xir_instance_start(foreign,fixture.probe,&argument,1)==XR_XIR_CALL_READY);
    XrXirInstanceResult result={0};
    for (unsigned i=0;i<1000;++i) {
        result=xr_xir_instance_poll_bounded(foreign,1);
        if (result.outcome.status!=XR_XIR_CALL_READY) break;
    }
    CHECK(result.outcome.status==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_free(foreign)==XR_XIR_CALL_BAD_ARGUMENT);foreign=NULL;
    XrXirVmDebug *borrowed=fixture.debug;
    CHECK(xdap_current_new(fixture.metadata,&fixture.debug,fixture.program,&config,&fixture.session)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_vm_debug_instance_new(borrowed,fixture.program,&config,&foreign)==XR_XIR_CALL_BAD_ARGUMENT && !foreign);
    start(&fixture);xdap_current_pause(fixture.session);
    XdapCurrentResult stopped=poll_fixture(&fixture);CHECK(stopped.state==XDAP_CURRENT_STOPPED);
    XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(fixture.session,&snapshot)==XR_XIR_VALUE_OK);
    XrXirVmDebugSnapshot *same=snapshot;
    CHECK(xdap_current_snapshot(fixture.session,&same)==XR_XIR_VALUE_BAD_ARGUMENT && same==snapshot);
    XrXirVmDebugFrameInfo info={.id=999};XrXirValue occupied={XR_XIR_I64,0,7};
    CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,0,&info,&occupied)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(info.id==999 && occupied.payload==7);
    CHECK(xdap_current_continue(fixture.session,stopped.debug_epoch,XR_XIR_VM_DEBUG_PAUSE)==XR_XIR_VALUE_BAD_ARGUMENT);
    xdap_current_result_drop(&stopped);fixture_free(&fixture);
    xr_xir_vm_debug_snapshot_drop(snapshot);fixture_dispose(&fixture,lowered);
    XrXirDomainBudgetControls limited={512,512,100000,1048576,1048576};
    XrXirDomain *domain=NULL;XrXirVmDebug *debug=NULL;
    CHECK(xr_xir_domain_new_budgeted(1048576,&limited,&domain)==XR_XIR_VALUE_OK);
    for (unsigned i=0;i<2;++i) {
        XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(domain);
        CHECK(xr_xir_vm_debug_new(domain,64,&debug)==XR_XIR_VALUE_LIMIT && !debug);
        XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(domain);
        CHECK(!after.metadata_live && after.requested_call_bytes>=before.requested_call_bytes && after.work>=before.work);
    }
    xr_xir_domain_drop(domain);CHECK(!runtime_live && !runtime_bytes);
}


static void execution_capacity_limit(void) {
    Fixture fixture;fixture_new_capacity(&fixture,1);
    start(&fixture);XdapCurrentResult result=poll_fixture(&fixture);
    CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_LIMIT);
    xdap_current_result_drop(&result);
    CHECK(xdap_current_free(fixture.session)==XR_XIR_CALL_LIMIT);fixture.session=NULL;
    fixture_dispose(&fixture,NULL);
}

int main(int argc,char **argv) {
    xr_test_suppress_dialogs();
    if (argc==2 && !strcmp(argv[1],"--resources")) { allocation_failures(); return 0; }
    CHECK(argc==1);
    puts("breakpoint_and_escape");breakpoint_and_escape();
    puts("stepping_and_cancel");stepping_and_cancel();
    puts("pause_and_step_over");pause_and_step_over();
    puts("admission_rejections");admission_rejections();
    puts("execution_capacity_limit");execution_capacity_limit();
    puts("Current Source/VM breakpoint, owned stack/values, step, resume/cancel, producer death: PASS");
    return 0;
}
