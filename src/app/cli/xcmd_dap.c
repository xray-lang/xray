/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_dap.c - Current DAP stdio entry on the canonical Source compiler
 */
#include "xcli.h"
#include "../../base/xresource_limits.h"
#include "../dap/xdap_dispatch.h"
#include "../dap/xdap_stdio.h"
#include "../dap/xdap_tcp.h"
#include "xcli_spec.h"
#include "xcli_dap_worker.h"
#include "../../api/xsource_paths.h"
#include "../../execution/xr_xir_host_time.h"
#include "../../xir/xxir_value_internal.h"
#include "../../os/os_time.h"
#include <stdio.h>

typedef struct DapHost {
    void *context;
    XdapDetachCommit commit;
    XrXirStatus (*retire_control)(void *);
    XrXirOutputSink sink;
    const char *stdout_path,*stderr_path;
    uint64_t process_id;
} DapHost;
#ifdef XR_OS_WINDOWS
#include "xcmd_dap_process_win.inc.c"
#endif
typedef struct DapEndpoint { XdapStdio *stdio;XdapTcp *tcp; } DapEndpoint;
static bool dap_port(const XrCliInvocation *inv,uint32_t *output) {
    const char *text=xr_cli_opt_string(&inv->options,"port",NULL);*output=0;
    if (!text) return true;
    if (!*text) return false;
    for (size_t i=0;text[i];++i) {
        if (i>=5 || text[i]<'0' || text[i]>'9') return false;
        *output=*output*10+(uint32_t)(text[i]-'0');if (*output>65535) return false;
    }
    return true;
}
static XrXirStatus dap_read(DapEndpoint *io,char **message,size_t *length,bool *eof) {
    return io->tcp ? xdap_tcp_read(io->tcp,message,length,eof) : xdap_stdio_read(io->stdio,message,length,eof);
}
static XrXirStatus dap_wait(DapEndpoint *io) {
    return io->tcp ? xdap_tcp_wait(io->tcp,10) : xdap_stdio_wait(io->stdio,10);
}
static XrXirStatus dap_write(void *context,const char *bytes,size_t length) {
    DapEndpoint *io=context;return io->tcp ? xdap_tcp_write(io->tcp,bytes,length) : xdap_stdio_write(io->stdio,bytes,length);
}
static int dap_loop(XdapAdapter *adapter,DapEndpoint *io) {
    while (!xdap_adapter_closed(adapter)) {
        char *message=NULL;size_t length=0;bool eof=false;
        XrXirStatus status=dap_read(io,&message,&length,&eof);
        if (status!=XR_XIR_OK) { xr_compile_resources_free(message);return 1; }
        if (message) {
            status=xdap_adapter_request(adapter,message,length);xr_compile_resources_free(message);
            if (status!=XR_XIR_OK) return 1;
        } else if (eof) return 0;
        if (xdap_adapter_running(adapter)) {
            if (xdap_adapter_step(adapter,32)!=XR_XIR_OK) return 1;
            if (xdap_adapter_waiting(adapter) && dap_wait(io)!=XR_XIR_OK) return 1;
        } else if (!message && !xdap_adapter_closed(adapter)) {
            if (dap_wait(io)!=XR_XIR_OK) return 1;
        }
    }
    return 0;
}
static int dap_detached_loop(XdapDetached *target) {
    bool ended=false;
    while (!ended) {
        if (xdap_detached_poll(target,32,&ended)!=XR_XIR_OK) return XR_CLI_EXIT_FAIL;
        if (xdap_detached_waiting(target)) xr_time_sleep_ms(1);
    }
    XrXirCallResult result={0};XrXirValueStatus copied=xdap_detached_result(target,&result);
    bool success=copied==XR_XIR_VALUE_OK && result.status==XR_XIR_CALL_RETURNED;
    xr_xir_call_result_drop(&result);return success ? XR_CLI_EXIT_OK : XR_CLI_EXIT_FAIL;
}
static int dap_run(bool tcp,uint32_t port,XrCompileResources *resources,DapHost *host) {
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    XrStdlibPath stdlib={0};XrSourcePathsDiagnostic path_diagnostic={0};
    XrXirDomain *domain=NULL;XdapAdapter *adapter=NULL;XdapDetached *detached=NULL;DapEndpoint io={0};
    XrXirOutputSink detached_sink={XR_XIR_CALL_ABI_VERSION,0,xdap_stdio_host_output,NULL,1048576};
    int result=XR_CLI_EXIT_INTERNAL;
    if (xr_source_stdlib_path(resources,&stdlib,&path_diagnostic)!=XR_SOURCE_COMPILE_OK) {
        char text[4096];
        xr_source_paths_diagnostic_format(&path_diagnostic,text,sizeof(text));
        fprintf(stderr,"DAP: %s\n",text);goto done;
    }
    XrSourceCompilerConfig compiler_config={{resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},stdlib.path,xr_source_compile_default_manifest_limits()};
    if (xr_source_compiler_new(&compiler_config,NULL,&compiler,&diagnostic)!=XR_SOURCE_COMPILE_OK) goto done;
    XrXirDomainBudgetControls budget={XR_RESOURCE_DEBUG_LIVE,XR_RESOURCE_DEBUG_LIVE,
        XR_RESOURCE_SANDBOX_RUNTIME_WORK,XR_RESOURCE_DEBUG_LIVE,XR_RESOURCE_DEBUG_LIVE};
    if (xr_xir_domain_new_budgeted(1048576,&budget,&domain)!=XR_XIR_VALUE_OK) goto done;
    if (tcp) {
        if (xdap_tcp_new(resources,(uint32_t)port,1048576,&io.tcp)!=XR_XIR_OK) goto done;
        fprintf(stderr,"DAP: listening on 127.0.0.1:%u\n",xdap_tcp_port(io.tcp));fflush(stderr);
    } else if (xdap_stdio_new(resources,1048576,&io.stdio)!=XR_XIR_OK) goto done;
    XdapAdapterConfig config={.observer_metadata=domain,.write=dap_write,.context=&io,.message_limit=1048576};
    if (xr_xir_instance_config_init(&config.instance,sizeof(config.instance))!=XR_XIR_CALL_READY) goto done;
    xr_xir_host_time_provider(&config.instance.time);
    if (tcp || host) {
        config.detached=&detached;
        config.instance.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,host ? &host->sink : &detached_sink};
        if (host) {
            config.detach_commit=host->commit;config.detach_context=host->context;config.retain_output=true;
            config.stdout_path=host->stdout_path;config.stderr_path=host->stderr_path;
            config.process_id=host->process_id;
        }
    }
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=XR_RESOURCE_DEBUG_LIVE;
    config.instance.requested_value_limit=config.instance.requested_call_limit=XR_RESOURCE_DEBUG_LIVE;
    config.instance.work_limit=XR_RESOURCE_SANDBOX_RUNTIME_WORK;
    config.instance.poll_limit=XR_RESOURCE_DEBUG_POLLS;config.instance.depth_limit=XR_RESOURCE_DEBUG_DEPTH;
    if (xdap_adapter_new(&compiler,&config,&adapter)!=XR_XIR_OK) goto done;
    result=dap_loop(adapter,&io) ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
    if (detached) {
        if (xdap_adapter_free(adapter)!=XR_XIR_CALL_READY) result=XR_CLI_EXIT_FAIL;
        adapter=NULL;xdap_stdio_free(io.stdio);io.stdio=NULL;xdap_tcp_free(io.tcp);io.tcp=NULL;
        if (host && host->retire_control(host->context)!=XR_XIR_OK) result=XR_CLI_EXIT_FAIL;
        if (dap_detached_loop(detached)!=XR_CLI_EXIT_OK) result=XR_CLI_EXIT_FAIL;
    }

done:
    if (xdap_adapter_free(adapter)!=XR_XIR_CALL_READY) result=XR_CLI_EXIT_FAIL;
    if (xdap_detached_free(detached)!=XR_XIR_CALL_READY) result=XR_CLI_EXIT_FAIL;
    xr_source_compiler_free(compiler);xr_source_compile_diagnostic_free(&diagnostic);
    xdap_stdio_free(io.stdio);xdap_tcp_free(io.tcp);xr_xir_domain_drop(domain);xr_source_stdlib_path_free(&stdlib);
    xr_source_paths_diagnostic_free(&path_diagnostic);
    if (result!=XR_CLI_EXIT_OK) fputs("DAP: protocol, source admission, resource or cleanup failure\n",stderr);
    return result;
}
XR_FUNC int cmd_dap(const XrCliInvocation *inv) {
    bool tcp=xr_cli_opt_present(&inv->options,"port");uint32_t port=0;
    if (!dap_port(inv,&port) || (tcp && xr_cli_opt_bool(&inv->options,"stdio"))) {
        fputs("DAP: select stdio or a TCP port from 0 to 65535\n",stderr);return XR_CLI_EXIT_USAGE;
    }
#ifndef XR_ARCH_X86_64
    fputs("DAP: this host has no supported XIR execution target\n",stderr);return XR_CLI_EXIT_FAIL;
#endif
#ifdef XR_OS_WINDOWS
    if (!tcp) return dap_process_frontend();
#endif
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits=(XrCompileResourceLimits)XR_RESOURCE_SANDBOX_COMPILER_LIMITS;
    if (xr_compile_resources_new(&limits,&resources)!=XR_COMPILE_RESOURCE_OK) return XR_CLI_EXIT_INTERNAL;
    int result=dap_run(tcp,port,resources,NULL);xr_compile_resources_release(resources);return result;
}
#ifdef XR_OS_WINDOWS
XR_FUNC int xr_cli_dap_worker_main(int argc,char **argv) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits=(XrCompileResourceLimits)XR_RESOURCE_SANDBOX_COMPILER_LIMITS;
    if (xr_compile_resources_new(&limits,&resources)!=XR_COMPILE_RESOURCE_OK) return XR_CLI_EXIT_INTERNAL;
    DapHost host={0};int result=XR_CLI_EXIT_FAIL;
    if (dap_process_worker_open(resources,argc,argv,&host)==XR_XIR_OK) result=dap_run(false,0,resources,&host);
    if (dap_process_worker_close(&host)!=XR_XIR_OK) result=XR_CLI_EXIT_FAIL;
    xr_compile_resources_release(resources);return result;
}
#endif
