/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_types.c - Real closed wrapper types and bounded display failures
 */
#include "app/dap/xdap_launch.h"
#include "app/dap/xdap_value.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_types.h"
#include "../../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"type display FAIL %d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../../xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
typedef struct TypeFixture {
    XrCompileResources *resources;
    XrCompileResourceStats baseline;
    XrSourceCompiler *compiler;
    XrXirDomain *domain;
    XdapLaunch *launch;
    XrXirVmDebugSnapshot *snapshot;
    uint64_t epoch;
} TypeFixture;
static void types_open(TypeFixture *fixture,const char *text,bool at_return) {
    *fixture=(TypeFixture){0};XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture->resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture->resources,&fixture->baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture->resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,NULL,&fixture->compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);
    xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture->domain)==XR_XIR_VALUE_OK);
    XrSourceCompilerText input={NULL,"dap-type-display",text,text ? strlen(text) : 0};
    XdapLaunchRequest request={.compiler=fixture->compiler,.text=text ? &input : NULL,
        .absolute_file=text ? NULL : XR_DAP_PRIVATE_FIXTURE,.observer_metadata=fixture->domain};
    CHECK(xr_xir_instance_config_init(&request.instance,sizeof(request.instance))==XR_XIR_CALL_READY);
    request.instance.metadata_limit=request.instance.value_limit=request.instance.call_limit=1048576;
    request.instance.requested_value_limit=request.instance.requested_call_limit=1048576;
    request.instance.work_limit=128000000;request.instance.poll_limit=10000;request.instance.depth_limit=64;
    XrXirStatus status=xdap_launch_build(&request,&fixture->launch,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"type source status=%u stage=%u: %s\n",status,
        diagnostic.source.stage,diagnostic.source.source.message);
    CHECK(status==XR_XIR_OK && fixture->launch);xr_source_compile_diagnostic_free(&diagnostic);
    if (text && strstr(text,"Atomic<i64>")) {
        const XrXirSourceView *view=xr_xir_compile_source_product_view(fixture->launch->source);
        CHECK(view && view->complete && view->module_count>fixture->launch->module_count);
        for (uint32_t m=0;m<fixture->launch->module_count;++m) {
            const XdapCodeModule *module=&fixture->launch->modules[m];bool found=false;
            for (uint32_t q=0;q<view->module_count;++q) if (!strcmp(module->identity,view->modules[q].identity)) found=true;
            CHECK(found);
        }
    }
    XdapFunctionSelection selection={.name="leaf",.length=4};XrXirVmDebugRule rules[32];uint32_t count=0;
    CHECK(xdap_launch_function_rules(fixture->launch,&selection,rules,32,&count)==XR_XIR_OK && count);
    if (at_return) for (uint32_t i=0;i<count;++i) {
        const XdapCodeFunction *function=&fixture->launch->functions[rules[i].location.function];uint32_t at=UINT32_MAX;
        for (uint32_t j=0;j<function->instructions;++j)
            if (fixture->launch->instructions[function->first+j].op==XR_XIR_RETURN) at=j;
        CHECK(at!=UINT32_MAX);rules[i].location.instruction=at;
    }
    CHECK(xdap_current_rules_replace(fixture->launch->control,rules,count)==XR_XIR_VALUE_OK);
    CHECK(xdap_current_start(fixture->launch->control,fixture->launch->entry,NULL,0)==XR_XIR_CALL_READY);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(fixture->launch->control,10000,&result)==XR_XIR_VALUE_OK);
    CHECK(result.state==XDAP_CURRENT_STOPPED && result.debug_epoch);fixture->epoch=result.debug_epoch;xdap_current_result_drop(&result);
    CHECK(xdap_current_snapshot(fixture->launch->control,&fixture->snapshot)==XR_XIR_VALUE_OK);
}
static void types_retire(TypeFixture *fixture) {
    CHECK(xdap_launch_free(fixture->launch)==XR_XIR_CALL_READY);fixture->launch=NULL;
    xr_source_compiler_free(fixture->compiler);fixture->compiler=NULL;
}
static void types_close(TypeFixture *fixture) {
    if (fixture->launch) types_retire(fixture);
    xr_xir_vm_debug_snapshot_drop(fixture->snapshot);
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static void types_assert(TypeFixture *fixture,const XrXirValue *value,const char *expected) {
    XdapValueView view={value,fixture->domain,fixture->snapshot,0};XdapTypeText output,before;
    memset(&output,0x5a,sizeof(output));before=output;
    XrXirDomainBudgetStats budget=xr_xir_domain_budget_stats(fixture->domain);
    size_t allocations=runtime_attempts;XrXirValueStatus status=xdap_value_type_text(&view,&output);
    CHECK(runtime_attempts==allocations);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(fixture->domain);
    CHECK(after.work>budget.work && after.metadata_live==budget.metadata_live && after.requested_bytes==budget.requested_bytes);
    if (expected) CHECK(status==XR_XIR_VALUE_OK && output.length==strlen(expected) && !memcmp(output.bytes,expected,output.length));
    else {
        CHECK(status==XR_XIR_VALUE_LIMIT && !memcmp(&output,&before,sizeof(output)));
        CHECK(xdap_value_type_text(&view,&output)==XR_XIR_VALUE_LIMIT && !memcmp(&output,&before,sizeof(output)));
        CHECK(xr_xir_domain_budget_stats(fixture->domain).work>after.work);
    }
}
static void types_display(TypeFixture *fixture,uint32_t slot,const char *expected) {
    XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_value(fixture->snapshot,0,slot,&value)==XR_XIR_VALUE_OK);
    types_assert(fixture,&value,expected);xr_xir_value_drop(&value);
}
static void types_append(char *text,size_t capacity,const char *part) {
    size_t used=strlen(text),length=strlen(part);CHECK(used<capacity && length<capacity-used);
    memcpy(text+used,part,length+1);
}
static void types_depth(uint32_t depth) {
    char text[4096]="fn leaf(value:",expected[512]="";
    for (uint32_t i=0;i<depth;++i) { types_append(text,sizeof(text),"Array<");types_append(expected,sizeof(expected),"Array<"); }
    types_append(text,sizeof(text),"i64");types_append(expected,sizeof(expected),"i64");
    for (uint32_t i=0;i<depth;++i) { types_append(text,sizeof(text),">");types_append(expected,sizeof(expected),">"); }
    types_append(text,sizeof(text),")->i64{return 0}\nleaf([])\n");
    TypeFixture fixture;types_open(&fixture,text,false);types_display(&fixture,0,depth<=32 ? expected : NULL);
    types_retire(&fixture);types_display(&fixture,0,depth<=32 ? expected : NULL);types_close(&fixture);
}
static void types_length(uint32_t members) {
    char text[16384]="fn leaf(value:(",expected[2048]="(";
    for (uint32_t i=0;i<members;++i) {
        types_append(text,sizeof(text),i ? ",i64" : "i64");types_append(expected,sizeof(expected),i ? ", i64" : "i64");
    }
    types_append(text,sizeof(text),"))->i64{return 0}\nleaf((");types_append(expected,sizeof(expected),")");
    for (uint32_t i=0;i<members;++i) types_append(text,sizeof(text),i ? ",0" : "0");
    types_append(text,sizeof(text),"))\n");
    TypeFixture fixture;types_open(&fixture,text,false);types_display(&fixture,0,strlen(expected)<=XDAP_TYPE_TEXT_BYTES ? expected : NULL);
    types_retire(&fixture);types_display(&fixture,0,strlen(expected)<=XDAP_TYPE_TEXT_BYTES ? expected : NULL);types_close(&fixture);
}
static void types_cell(void) {
    TypeFixture fixture;types_open(&fixture,
        "fn leaf()->fn()->i64 {var held=1;return fn()->i64{held=held+1;return held}}\nleaf()\n",true);
    XrXirVmDebugFrameInfo frame={0};XrXirValue name={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(fixture.snapshot,0,&frame,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
    uint32_t slot=UINT32_MAX;
    for (uint32_t i=0;i<frame.value_count;++i) {
        XrXirValue value={0};XrXirValueStatus status=xr_xir_vm_debug_snapshot_value(fixture.snapshot,0,i,&value);
        if (status==XR_XIR_VALUE_OK) {
            const XrXirTypes *types=xr_xir_compile_type_arena_types(xr_xir_value_arena(&value));
            if (xr_xir_type_is_cell(types,(XrXirType)value.type)) slot=i;
        }
        xr_xir_value_drop(&value);
    }
    CHECK(slot!=UINT32_MAX);types_display(&fixture,slot,"Cell<i64>");
    types_retire(&fixture);types_display(&fixture,slot,"Cell<i64>");types_close(&fixture);
}
static void types_next(TypeFixture *fixture) {
    CHECK(xdap_current_continue(fixture->launch->control,fixture->epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
    xr_xir_vm_debug_snapshot_drop(fixture->snapshot);fixture->snapshot=NULL;
    XdapCurrentResult result={0};CHECK(xdap_current_poll(fixture->launch->control,10000,&result)==XR_XIR_VALUE_OK);
    if (result.state!=XDAP_CURRENT_STOPPED || result.debug_epoch<=fixture->epoch)
        fprintf(stderr,"Private throw transition: state=%u epoch=%llu previous=%llu outcome=%u\n",
            result.state,(unsigned long long)result.debug_epoch,(unsigned long long)fixture->epoch,result.outcome.status);
    CHECK(result.state==XDAP_CURRENT_STOPPED && result.debug_epoch>fixture->epoch);
    fixture->epoch=result.debug_epoch;xdap_current_result_drop(&result);
    CHECK(xdap_current_snapshot(fixture->launch->control,&fixture->snapshot)==XR_XIR_VALUE_OK);
}
static void types_private(void) {
    TypeFixture fixture;types_open(&fixture,NULL,false);
    CHECK(xdap_current_break_on_throw(fixture.launch->control,true)==XR_XIR_VALUE_OK);
    types_next(&fixture);XrXirValue owner={0};
    CHECK(xr_xir_vm_debug_snapshot_exception(fixture.snapshot,&owner)==XR_XIR_VALUE_OK);
    types_assert(&fixture,&owner,"Hidden");xr_xir_value_drop(&owner);
    types_next(&fixture);XrXirValue caller={0};
    CHECK(xr_xir_vm_debug_snapshot_exception(fixture.snapshot,&caller)==XR_XIR_VALUE_OK);
    types_assert(&fixture,&caller,"<private type>");
    types_retire(&fixture);types_assert(&fixture,&caller,"<private type>");
    xr_xir_value_drop(&caller);types_close(&fixture);
}
int main(void) {
    xr_test_suppress_dialogs();TypeFixture fixture;
    const char source[]="struct Box<T>{value:T}\nfn worker()->i64{return 7}\n"
        "fn leaf(box:Box<(i64,string)>,atomic:Atomic<i64>,task:Task<i64>)->i64{return 0}\n"
        "leaf(Box<(i64,string)>{value:(1,\"owned\")},Atomic<i64>(2),go worker())\n";
    types_open(&fixture,source,false);
    const char *expected[]={"Box<(i64, string)>","Atomic<i64>","Task<i64>"};
    for (uint32_t i=0;i<3;++i) types_display(&fixture,i,expected[i]);
    types_retire(&fixture);for (uint32_t i=0;i<3;++i) types_display(&fixture,i,expected[i]);types_close(&fixture);
    types_cell();types_private();types_depth(32);types_depth(33);types_length(204);types_length(205);
    puts("Closed generic/Atomic/Task/Cell types, authentic private names, depth32/33 and text1020/1025, escape and physical 0/0: PASS");return 0;
}
