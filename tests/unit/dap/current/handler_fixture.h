/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * handler_fixture.h - Real Source, output and physical ownership fixture
 */
#ifndef DAP_HANDLER_FIXTURE_H
#define DAP_HANDLER_FIXTURE_H
#include "xir/xxir_output.h"
typedef struct HandlerFixture {
    XrCompileResources *resources;
    XrSourceCompiler *compiler;
    XrXirDomain *domain;
    XrCompileResourceStats baseline;
    XrSourceCompileDiagnostic diagnostic;
    XdapLaunch *launch;
    XrXirOutputSink sink;
    char output[128];
    size_t length;
} HandlerFixture;
static XrXirOutputStatus handler_output(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    HandlerFixture *fixture=context;
    CHECK(stream==XR_XIR_STDOUT && length<sizeof(fixture->output)-fixture->length);
    memcpy(fixture->output+fixture->length,bytes,length);fixture->length+=length;return XR_XIR_OUTPUT_OK;
}
static void handler_open(HandlerFixture *fixture) {
    XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture->resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture->resources,&fixture->baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture->resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    CHECK(xr_source_compiler_new(&source,&fixture->compiler,&fixture->diagnostic)==XR_SOURCE_COMPILE_OK);
    xr_source_compile_diagnostic_free(&fixture->diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture->domain)==XR_XIR_VALUE_OK);
}
static XrXirStatus handler_build(HandlerFixture *fixture,const char *text) {
    XrSourceCompilerText input={NULL,"dap-handler-sites",text,strlen(text)};
    XdapLaunchRequest request={.compiler=fixture->compiler,.text=&input,.observer_metadata=fixture->domain};
    CHECK(xr_xir_instance_config_init(&request.instance,sizeof(request.instance))==XR_XIR_CALL_READY);
    request.instance.metadata_limit=request.instance.value_limit=request.instance.call_limit=1048576;
    request.instance.requested_value_limit=request.instance.requested_call_limit=1048576;
    request.instance.work_limit=128000000;request.instance.poll_limit=10000;request.instance.depth_limit=64;
    fixture->sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,handler_output,fixture,sizeof(fixture->output)};
    request.instance.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&fixture->sink};
    return xdap_launch_build(&request,&fixture->launch,&fixture->diagnostic);
}
static void handler_close(HandlerFixture *fixture) {
    CHECK(xdap_launch_free(fixture->launch)==XR_XIR_CALL_READY);fixture->launch=NULL;
    xr_source_compile_diagnostic_free(&fixture->diagnostic);xr_source_compiler_free(fixture->compiler);
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};
    CHECK(xr_compile_resources_stats(fixture->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static uint32_t handler_line(const char *text,const char *marker) {
    const char *found=strstr(text,marker);CHECK(found && !strstr(found+strlen(marker),marker));
    uint32_t line=1;for (const char *at=text;at<found;++at) if (*at=='\n') ++line;return line;
}
#endif // DAP_HANDLER_FIXTURE_H
