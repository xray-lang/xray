/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_dispatch_faults.c - Request ownership and atomic restart failure prefixes
 */
#define main reference_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
static const char launch_request[]="{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"" XR_DAP_FIXTURE "\"}}";
static const char restart_request[]="{\"seq\":5,\"type\":\"request\",\"command\":\"restart\"}";
static void fault_setup(XrCompileResources **resources,XrSourceCompiler **compiler,XrXirDomain **domain,
    XdapAdapterConfig *config,XrCompileResourceStats *baseline) {
    memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(*resources,baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{*resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompileDiagnostic diagnostic={0};CHECK(xr_source_compiler_new(&source,compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);
    xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,domain)==XR_XIR_VALUE_OK);
    *config=(XdapAdapterConfig){.observer_metadata=*domain,.write=capture_message,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config->instance,sizeof(config->instance))==XR_XIR_CALL_READY);
    config->instance.metadata_limit=config->instance.value_limit=config->instance.call_limit=1048576;
    config->instance.requested_value_limit=config->instance.requested_call_limit=1048576;
    config->instance.work_limit=128000000;config->instance.poll_limit=10000;config->instance.depth_limit=64;
}
static void fault_prime(XdapAdapter *adapter,unsigned mode) {
    request(adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    if (mode==1) return;
    request(adapter,launch_request);XrJsonValue *reply=last_response("launch",true);xjson_free(reply);
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"leaf\"}]}}");
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);
}
static size_t fault_operation(unsigned mode,size_t fail) {
    XrCompileResources *resources=NULL;XrSourceCompiler *compiler=NULL;XrXirDomain *domain=NULL;
    XdapAdapterConfig config={0};XrCompileResourceStats baseline={0};
    fault_setup(&resources,&compiler,&domain,&config,&baseline);XdapAdapter *adapter=NULL;
    if (mode) { CHECK(xdap_adapter_new(&compiler,&config,&adapter)==XR_XIR_OK && !compiler);
        capture.adapter=adapter;capture.reentry=true;fault_prime(adapter,mode); }
    XdapLaunch *old=adapter ? adapter->launch : NULL;
    XrXirVmDebugSnapshot *snapshot=adapter ? adapter->snapshot : NULL;
    uint64_t epoch=adapter ? adapter->stop_epoch : 0;
    XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    uint32_t messages=capture.count;size_t begin=runtime_attempts;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;XrXirStatus status;
    if (!mode) status=xdap_adapter_new(&compiler,&config,&adapter);
    else status=xdap_adapter_request(adapter,mode==1 ? launch_request :
        restart_request,mode==1 ? sizeof(launch_request)-1 : sizeof(restart_request)-1);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) CHECK(status==XR_XIR_OK);
    else {
        CHECK(sites>fail);
        if (!mode) { CHECK(status==XR_XIR_OUT_OF_MEMORY && compiler && !adapter);
            CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);
            CHECK(xdap_adapter_new(&compiler,&config,&adapter)==XR_XIR_OK && !compiler); }
        else {
            CHECK(status==XR_XIR_OUT_OF_MEMORY || status==XR_XIR_OK);
            if (status==XR_XIR_OK) { CHECK(capture.count>messages);
                XrJsonValue *reply=last_response(mode==1 ? "launch" : "restart",false);xjson_free(reply); }
            if (mode==2) {
                CHECK(adapter->launch==old && adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running);
                XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_value(snapshot,0,0,&value)==XR_XIR_VALUE_OK);
                CHECK(value.type==XR_XIR_I64 && value.payload==20);xr_xir_value_drop(&value);
            }
        }
    }
    if (mode==2 && fail==SIZE_MAX) {
        CHECK(adapter->launch!=old && !adapter->snapshot && adapter->running);
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && adapter->stop_epoch);
    }
    XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(resources,&spent)==XR_COMPILE_RESOURCE_OK);
    CHECK(spent.allocated_bytes>=before.allocated_bytes && spent.work>=before.work);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);xr_source_compiler_free(compiler);
    CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool inject=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || inject);
    for (unsigned mode=0;mode<3;++mode) {
        size_t sites=fault_operation(mode,SIZE_MAX);CHECK(sites);
        if (inject) for (size_t i=0;i<sites;++i) fault_operation(mode,i);
        printf("DAP request allocation slice %u: %zu sites, injection=%s; physical=0/0\n",mode,sites,inject ? "ALL" : "NOT_RUN");
    }
    return 0;
}
