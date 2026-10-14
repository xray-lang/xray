/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_dap.c - Current DAP stdio entry on the canonical Source compiler
 */
#include "xcli.h"
#include "../dap/xdap_dispatch.h"
#include "../dap/xdap_stdio.h"
#include "../../api/xsource_paths.h"
#include "../../execution/xr_xir_host_time.h"
#include "../../xir/xxir_value_internal.h"
#include <stdio.h>
static int dap_loop(XdapAdapter *adapter,XdapStdio *io) {
    while (!xdap_adapter_closed(adapter)) {
        char *message=NULL;size_t length=0;bool eof=false;
        XrXirStatus status=xdap_stdio_read(io,&message,&length,&eof);
        if (status!=XR_XIR_OK) { xr_compile_resources_free(message);return 1; }
        if (message) {
            status=xdap_adapter_request(adapter,message,length);xr_compile_resources_free(message);
            if (status!=XR_XIR_OK) return 1;
        } else if (eof) return 0;
        if (xdap_adapter_running(adapter)) {
            if (xdap_adapter_step(adapter,32)!=XR_XIR_OK) return 1;
            if (xdap_adapter_waiting(adapter) && xdap_stdio_wait(io,10)!=XR_XIR_OK) return 1;
        } else if (!message && !xdap_adapter_closed(adapter)) {
            if (xdap_stdio_wait(io,10)!=XR_XIR_OK) return 1;
        }
    }
    return 0;
}
XR_FUNC int cmd_dap(const XrCliInvocation *inv) {
    (void)inv;
#ifndef XR_ARCH_X86_64
    fputs("DAP: this host has no supported XIR execution target\n",stderr);return XR_CLI_EXIT_FAIL;
#endif
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits=xr_source_compile_default_resource_limits();
    if (xr_compile_resources_new(&limits,&resources)!=XR_COMPILE_RESOURCE_OK) return XR_CLI_EXIT_INTERNAL;
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    XrStdlibPath stdlib={0};XrSourcePathsDiagnostic path_diagnostic={0};
    XrXirDomain *domain=NULL;XdapAdapter *adapter=NULL;XdapStdio *io=NULL;
    int result=XR_CLI_EXIT_INTERNAL;
    if (xr_source_stdlib_path(resources,&stdlib,&path_diagnostic)!=XR_SOURCE_COMPILE_OK) goto done;
    XrSourceCompilerConfig compiler_config={{resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},stdlib.path,xr_source_compile_default_manifest_limits()};
    if (xr_source_compiler_new(&compiler_config,&compiler,&diagnostic)!=XR_SOURCE_COMPILE_OK) goto done;
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    if (xr_xir_domain_new_budgeted(1048576,&budget,&domain)!=XR_XIR_VALUE_OK) goto done;
    if (xdap_stdio_new(resources,1048576,&io)!=XR_XIR_OK) goto done;
    XdapAdapterConfig config={.observer_metadata=domain,.write=xdap_stdio_write,.context=io,.message_limit=1048576};
    if (xr_xir_instance_config_init(&config.instance,sizeof(config.instance))!=XR_XIR_CALL_READY) goto done;
    xr_xir_host_time_provider(&config.instance.time);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    if (xdap_adapter_new(&compiler,&config,&adapter)!=XR_XIR_OK) goto done;
    result=dap_loop(adapter,io) ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
done:
    if (xdap_adapter_free(adapter)!=XR_XIR_CALL_READY) result=XR_CLI_EXIT_FAIL;
    xr_source_compiler_free(compiler);xr_source_compile_diagnostic_free(&diagnostic);
    xdap_stdio_free(io);xr_xir_domain_drop(domain);xr_source_stdlib_path_free(&stdlib);
    xr_compile_resources_release(resources);
    if (result!=XR_CLI_EXIT_OK) fputs("DAP: protocol, source admission, resource or cleanup failure\n",stderr);
    return result;
}
