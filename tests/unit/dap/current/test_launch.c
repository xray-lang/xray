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
int main(void) {
    xr_test_suppress_dialogs();
    XrXirCompileContext context={0};XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig config={context,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,
        xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&config,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);
    const char text[]="fn leaf(value:i64)->string { var observed=value*2; const text=\"owned\\0value\"; "
        "observed=observed+1; return text }\nfn main()->string { return leaf(20) }\nmain()\n";
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
    CHECK(xdap_current_breakpoint(launch->control,leaf,at,true)==XR_XIR_VALUE_OK);
    xr_source_compiler_free(compiler);compiler=NULL;
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
    puts("Canonical SourceCompiler/Closed packet/observed VM launch and producer death: PASS");return 0;
}
