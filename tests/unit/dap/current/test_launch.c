/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_launch.c - SourceCompiler packets driving real observed VM execution
 */
#include "app/dap/xdap_launch.h"
#include "../../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../../xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "base/xcompile_state.h"
static size_t launch_fault(size_t fail) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig config={{resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&config,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);
    xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomain *domain=NULL;XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&domain)==XR_XIR_VALUE_OK);
    const char text[]="fn leaf(value:i64)->string { return \"owned\\0value\" }\nleaf(20)\n";
    XrSourceCompilerText input={NULL,"dap-launch-fault",text,sizeof(text)-1};
    XdapLaunchRequest request={.compiler=compiler,.text=&input,.observer_metadata=domain};
    CHECK(xr_xir_instance_config_init(&request.instance,sizeof(request.instance))==XR_XIR_CALL_READY);
    request.instance.metadata_limit=request.instance.value_limit=request.instance.call_limit=1048576;
    request.instance.requested_value_limit=request.instance.requested_call_limit=1048576;
    request.instance.work_limit=128000000;request.instance.poll_limit=10000;request.instance.depth_limit=64;
    XdapLaunch *launch=NULL;size_t begin=runtime_attempts;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_launch_build(&request,&launch,&diagnostic);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) CHECK(status==XR_XIR_OK && launch);
    else {
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"launch fail=%zu status=%u diagnostic=%u\n",fail,status,diagnostic.status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && !launch && sites>fail);
        CHECK(diagnostic.status==XR_SOURCE_COMPILE_OUT_OF_MEMORY);
        CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);
        XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(resources,&spent)==XR_COMPILE_RESOURCE_OK);
        bool sticky=xr_compile_state_status(xr_source_compiler_state(compiler))==XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
        xr_source_compile_diagnostic_free(&diagnostic);
        status=xdap_launch_build(&request,&launch,&diagnostic);
        CHECK(status==(sticky ? XR_XIR_OUT_OF_MEMORY : XR_XIR_OK));
        CHECK(sticky ? !launch : launch!=NULL);
        XrCompileResourceStats retry={0};CHECK(xr_compile_resources_stats(resources,&retry)==XR_COMPILE_RESOURCE_OK);
        CHECK(retry.allocated_bytes>=spent.allocated_bytes && retry.work>=spent.work);
    }
    CHECK(xdap_launch_free(launch)==XR_XIR_CALL_READY);xr_source_compile_diagnostic_free(&diagnostic);
    xr_source_compiler_free(compiler);CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();
    if (argc==2 && !strcmp(argv[1],"--resources")) {
        size_t sites=launch_fault(SIZE_MAX);CHECK(sites);
        for (size_t i=0;i<sites;++i) launch_fault(i);
        printf("Canonical launch full allocation faults=%zu; preserved output, diagnostic, original ledger retry and physical=0/0 PASS\n",sites);
        return 0;
    }
    CHECK(argc==1);
    XrXirCompileContext context={0};XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig config={context,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,
        xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&config,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);
    const char text[]="fn leaf(value:i64)->string {\n"
        "    var observed=value*2\n"
        "    const text=\"owned\\0value\"\n"
        "    observed=observed+1\n"
        "    return text\n"
        "}\n"
        "fn main()->string {\n"
        "    return leaf(20)\n"
        "}\nmain()\n";
    XrSourceCompilerText input={NULL,"dap-launch",text,sizeof(text)-1};
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};XrXirDomain *domain=NULL;
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&domain)==XR_XIR_VALUE_OK);
    XdapLaunchRequest request={.compiler=compiler,.text=&input,.observer_metadata=domain};
    CHECK(xr_xir_instance_config_init(&request.instance,sizeof(request.instance))==XR_XIR_CALL_READY);
    request.instance.metadata_limit=request.instance.value_limit=request.instance.call_limit=1048576;
    request.instance.requested_value_limit=request.instance.requested_call_limit=1048576;
    request.instance.work_limit=128000000;request.instance.poll_limit=10000;request.instance.depth_limit=64;
    XdapLaunch *launch=NULL;XrXirStatus status=xdap_launch_build(&request,&launch,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"launch=%u source=%u stage=%u: %s\n",status,diagnostic.status,
        diagnostic.source.stage,diagnostic.source.source.message);
    CHECK(status==XR_XIR_OK && launch);
    CHECK(launch->module_count);
    const XrXirSourceView *view=xr_xir_compile_source_product_view(launch->source);
    CHECK(view && view->complete && view->module_count==launch->module_count);
    for (uint32_t m=0;m<launch->module_count;++m) {
        bool found=false;const XdapCodeModule *module=&launch->modules[m];CHECK(module->identity && !module->path);
        for (uint32_t q=0;q<view->module_count;++q) if (!strcmp(module->identity,view->modules[q].identity)) {
            CHECK(module->identity!=view->modules[q].identity && !memcmp(&module->fingerprint,&view->modules[q].fingerprint,sizeof(module->fingerprint)));found=true;
        }
        CHECK(found);
    }
    uint32_t leaf=UINT32_MAX,at=UINT32_MAX,string=UINT32_MAX;
    for (uint32_t i=0;i<launch->function_count;++i) if (!strcmp(launch->functions[i].name,"leaf")) leaf=i;
    CHECK(leaf!=UINT32_MAX);
    XdapCodeFunction *function=&launch->functions[leaf];
    for (uint32_t i=0;i<function->instructions;++i) {
        XrXirInstruction *op=&launch->instructions[function->first+i];
        if (op->op==XR_XIR_RETURN) at=i;
        if (op->op==XR_XIR_CONST_STRING) string=function->parameters+i;
    }
    CHECK(at!=UINT32_MAX && string!=UINT32_MAX);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(launch->source);
    CHECK(debug && debug->function_count==launch->function_count);
    const XrXirSourceDebugFunction *mapped=&debug->functions[leaf];
    CHECK(mapped->function==leaf && mapped->parameter_count==function->parameters &&
        mapped->instruction_count==function->instructions && mapped->points);
    bool source_lines[6]={0};
    const int source_columns[6]={0,0,9,11,5,5};
    for (uint32_t i=0;i<mapped->instruction_count;++i) {
        const XrXirSourceDebugPoint *point=&mapped->points[i];
        if (!point->statement) continue;
        CHECK(point->range.module<view->module_count && point->range.line>=2 && point->range.line<=5);
        if (point->range.column!=source_columns[point->range.line])
            fprintf(stderr,"Source point i=%u line=%d column=%d\n",i,point->range.line,point->range.column);
        CHECK(point->range.column==source_columns[point->range.line]);source_lines[point->range.line]=true;
    }
    if (!(source_lines[2] && source_lines[3] && source_lines[4] && source_lines[5]))
        for (uint32_t i=0;i<mapped->instruction_count;++i)
            fprintf(stderr,"Mapped %u op=%u statement=%u line=%d column=%d\n",i,
                launch->instructions[function->first+i].op,mapped->points[i].statement,
                mapped->points[i].range.line,mapped->points[i].range.column);
    CHECK(source_lines[2] && source_lines[3] && source_lines[4] && source_lines[5]);
    CHECK(mapped->points[at].range.line==5 && mapped->points[at].statement);
    CHECK(mapped->points[string-function->parameters].range.line==3);
    CHECK(xdap_current_breakpoint(launch->control,leaf,at,true)==XR_XIR_VALUE_OK);
    xr_source_compiler_free(compiler);compiler=NULL;
    CHECK(xr_xir_compile_source_product_debug(launch->source)==debug && mapped->points[at].range.line==5);
    CHECK(xdap_current_start(launch->control,launch->entry,NULL,0)==XR_XIR_CALL_READY);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(launch->control,1000,&result)==XR_XIR_VALUE_OK);
    CHECK(result.state==XDAP_CURRENT_STOPPED);
    XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(launch->control,&snapshot)==XR_XIR_VALUE_OK);
    XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_value(snapshot,0,string,&value)==XR_XIR_VALUE_OK);
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&value,&bytes,&length) && length==11 && !memcmp(bytes,"owned\0value",11));
    xr_xir_value_drop(&value);
    CHECK(xdap_current_continue(launch->control,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
    xdap_current_result_drop(&result);CHECK(xdap_current_poll(launch->control,1000,&result)==XR_XIR_VALUE_OK);
    CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_RETURNED);
    xdap_current_result_drop(&result);CHECK(xdap_launch_free(launch)==XR_XIR_CALL_READY);
    CHECK(xr_xir_vm_debug_snapshot_value(snapshot,0,string,&value)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_view(&value,&bytes,&length) && length==11);xr_xir_value_drop(&value);
    xr_xir_vm_debug_snapshot_drop(snapshot);xr_source_compile_diagnostic_free(&diagnostic);
    CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(context.resources);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Canonical SourceCompiler/Closed packet/observed VM launch, owned sealed Source instruction maps and producer death: PASS");return 0;
}
