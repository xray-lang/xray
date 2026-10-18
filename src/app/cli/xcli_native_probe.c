/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_native_probe.c - Fresh native capability attempts with bounded fallback
 */
#include "xcli_native_probe.h"
#include "../../api/xsource_compiler.h"
#include "../../api/xsource_paths.h"
#include "../../shared/xr_path_limit.h"
#include "../toolchain/xtc_xir_native_admission.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

struct XrCliNativeProbe {
    XrCompileResources *resources;
    XtcXirLocalToolchain *local;
    XtcXirNativeOperation *operation;
    XrSourceCompiler *compiler;
    XrXirSourceProduct *product;
    XrXirNativeProjection *projection;
    XrXirNativeArtifact *artifact;
    XrSourceCompileDiagnostic source_diagnostic;
    XrStdlibPath stdlib;
    XrCliNativeProbeReport report;
    uint8_t compiler_digest[32], linker_digest[32], sdk_identity[32];
    uint64_t compiler_length, linker_length;
    XrCliNativeProbeStatus status;
    bool ran, retryable;
};
static XrCliNativeProbeStatus probe_resource(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK ? XR_CLI_NATIVE_PROBE_OK :
        status==XR_COMPILE_RESOURCE_BUDGET ? XR_CLI_NATIVE_PROBE_BUDGET :
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_CLI_NATIVE_PROBE_OUT_OF_MEMORY : XR_CLI_NATIVE_PROBE_INVALID;
}
static bool probe_fail(XrCliNativeProbe *owner,XrCliNativeProbeStatus status,
    const char *code,const char *stage,unsigned detail,int nested) {
    if (!owner->report.failure.code) {
        owner->status=status;owner->report.failure.code=code;owner->report.failure.stage=stage;
        snprintf(owner->report.failure.message,sizeof(owner->report.failure.message),
            "%s failed (status=%u detail=%d)",stage,detail,nested);
    }
    owner->report.ready=false;return false;
}
static bool probe_work(XrCliNativeProbe *owner,uint64_t units) {
    XrCompileResourceStatus status=xr_compile_resources_work(owner->resources,units);
    return status==XR_COMPILE_RESOURCE_OK || probe_fail(owner,probe_resource(status),"PROBE_BUDGET","probe-work",(unsigned)status,0);
}
static void probe_report_reset(XrCliNativeProbe *owner) {
    XrCliNativeProbeFailure previous=owner->report.previous;
    owner->report=(XrCliNativeProbeReport){0};owner->report.previous=previous;
    owner->report.provider="none";owner->report.c_compile=owner->report.sdk_compile=
        owner->report.runtime_link=owner->report.native_run="skipped";
    owner->status=XR_CLI_NATIVE_PROBE_OK;owner->retryable=false;
}
static XrCliNativeProbeStatus probe_operation_close(XrCliNativeProbe *owner) {
    XtcXirNativeOperationStatus status=xtc_xir_native_operation_close(&owner->operation,1024);
    if (status==XTC_XIR_NATIVE_OK) return XR_CLI_NATIVE_PROBE_OK;
    owner->report.ready=false;owner->retryable=false;
    return status==XTC_XIR_NATIVE_PENDING ? XR_CLI_NATIVE_PROBE_PENDING : XR_CLI_NATIVE_PROBE_CLEANUP;
}
static void probe_source_free(XrCliNativeProbe *owner) {
    xr_compile_native_artifact_free(owner->artifact);owner->artifact=NULL;
    xr_compile_native_projection_owner_free(owner->projection);owner->projection=NULL;
    xr_xir_compile_source_product_free(owner->product);owner->product=NULL;
    xr_source_compiler_free(owner->compiler);owner->compiler=NULL;
    xr_source_compile_diagnostic_free(&owner->source_diagnostic);
    xr_source_stdlib_path_free(&owner->stdlib);
}
static bool probe_source(XrCliNativeProbe *owner) {
    if (owner->projection) return true;
    XrSourcePathsDiagnostic path={0};XrSourceCompileStatus status=xr_source_stdlib_path(owner->resources,&owner->stdlib,&path);
    XrSourceCompilerConfig config={{owner->resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->stdlib.path,xr_source_compile_default_manifest_limits()};
    if (status==XR_SOURCE_COMPILE_OK) status=xr_source_compiler_new(&config,&owner->compiler,&owner->source_diagnostic);
    const XrSourceCompilerText text={NULL,"xray-toolchain-probe","print(42)\n",10};
    if (status==XR_SOURCE_COMPILE_OK) status=xr_source_compiler_build_text(owner->compiler,&text,&owner->product,&owner->source_diagnostic);
    if (status!=XR_SOURCE_COMPILE_OK) {
        XrCliNativeProbeStatus mapped=status==XR_SOURCE_COMPILE_BUDGET ? XR_CLI_NATIVE_PROBE_BUDGET :
            status==XR_SOURCE_COMPILE_OUT_OF_MEMORY ? XR_CLI_NATIVE_PROBE_OUT_OF_MEMORY : XR_CLI_NATIVE_PROBE_INTEGRITY;
        return probe_fail(owner,mapped,"COMPILE_PROBE_FAILED","source-program",(unsigned)status,(int)owner->source_diagnostic.stage);
    }
    XrXirNativeProjectionRequest project={owner->product,"xray_toolchain_probe",UINT64_C(64)<<20};
    XrXirStatus projected=xr_compile_native_projection_prepare(&project,&owner->projection,NULL);
    if (projected==XR_XIR_OK) return true;
    XrCliNativeProbeStatus mapped=projected==XR_XIR_BUDGET ? XR_CLI_NATIVE_PROBE_BUDGET :
        projected==XR_XIR_OUT_OF_MEMORY ? XR_CLI_NATIVE_PROBE_OUT_OF_MEMORY : XR_CLI_NATIVE_PROBE_INTEGRITY;
    return probe_fail(owner,mapped,"COMPILE_PROBE_FAILED","native-projection",(unsigned)projected,0);
}
static bool probe_execute(XrCliNativeProbe *owner) {
    const XrXirInvocationFacts *facts=xtc_xir_native_operation_facts(owner->operation);
    const XrProcessView *link=xtc_xir_native_operation_command(owner->operation,XR_XIR_INVOCATION_LINK);
    const XrXirInvocationFile *output=NULL;
    if (!facts || facts->completed_runs!=6 || !link || link->env_count>XTC_PROCESS_MAX_ENV)
        return probe_fail(owner,XR_CLI_NATIVE_PROBE_INTEGRITY,"PROBE_INPUT_INVALID","native-run-input",0,0);
    for (uint32_t i=0;i<facts->file_count;++i) {
        if (!probe_work(owner,1)) return false;
        const XrXirInvocationFile *file=xtc_xir_native_operation_file(owner->operation,i);
        if (file && file->kind==XR_XIR_INVOCATION_OUTPUT) {
            if (output) return probe_fail(owner,XR_CLI_NATIVE_PROBE_INTEGRITY,"PROBE_INPUT_INVALID","native-run-input",0,0);
            output=file;
        }
    }
    if (!output) return probe_fail(owner,XR_CLI_NATIVE_PROBE_INTEGRITY,"PROBE_INPUT_INVALID","native-run-input",0,0);
    XrProcessSpec spec;xtc_process_spec_init(&spec,output->path,30000);
    spec.environment_source=XTC_PROCESS_ENV_EXPLICIT;spec.cwd=link->cwd;spec.output_limit=4096;spec.env_count=link->env_count;
    for (size_t i=0;i<link->env_count;++i) { spec.env_keys[i]=link->env_keys[i];spec.env_values[i]=link->env_values[i]; }
    XrToolchainProcess *process=NULL;XrProcessResult result={0};
    XrProcessStatus status=xtc_process_prepare(owner->resources,&spec,&process);
    if (status==XTC_PROCESS_OK) status=xtc_process_run(process,NULL,NULL,&result);
    bool okay=status==XTC_PROCESS_OK && result.exit_code==0 && !result.stdout_bytes.truncated &&
        !result.stderr_bytes.truncated && result.stdout_bytes.length==3 &&
        !memcmp(result.stdout_bytes.data,"42\n",3) && !result.stderr_bytes.length;
    if (!okay) {
        XrCliNativeProbeStatus mapped=status==XTC_PROCESS_BUDGET ? XR_CLI_NATIVE_PROBE_BUDGET :
            status==XTC_PROCESS_OUT_OF_MEMORY ? XR_CLI_NATIVE_PROBE_OUT_OF_MEMORY : XR_CLI_NATIVE_PROBE_INTEGRITY;
        probe_fail(owner,mapped,"RUN_PROBE_FAILED","native-run",(unsigned)status,result.exit_code);
    }
    xtc_process_result_free(&result);xtc_process_free(process);return okay;
}
static bool probe_hex(XrCliNativeProbe *owner,const uint8_t bytes[32],char output[65]) {
    if (!probe_work(owner,161)) return false;
    static const char hex[]="0123456789abcdef";
    for (unsigned i=0;i<32;++i) { output[i*2]=hex[bytes[i]>>4];output[i*2+1]=hex[bytes[i]&15]; }
    output[64]=0;return true;
}
static bool probe_observe(XrCliNativeProbe *owner) {
    XtcXirNativeAdmissionDiagnostic diagnostic={0};
    XtcXirNativeAdmissionStatus admitted=xtc_xir_native_admit(owner->operation,owner->projection,
        xtc_xir_local_toolchain_sdk(owner->local),UINT64_C(64)<<20,&owner->artifact,&diagnostic);
    if (admitted!=XTC_XIR_ADMISSION_OK) {
        XrCliNativeProbeStatus mapped=admitted==XTC_XIR_ADMISSION_BUDGET ? XR_CLI_NATIVE_PROBE_BUDGET :
            admitted==XTC_XIR_ADMISSION_OUT_OF_MEMORY ? XR_CLI_NATIVE_PROBE_OUT_OF_MEMORY : XR_CLI_NATIVE_PROBE_INTEGRITY;
        return probe_fail(owner,mapped,"ABI_MISMATCH","native-admission",(unsigned)admitted,diagnostic.code);
    }
    const XrXirInvocationProviderFacts *provider=xtc_xir_native_operation_provider(owner->operation);
    const XrXirRuntimeSdkFacts *sdk=xr_xir_runtime_sdk_facts(xtc_xir_local_toolchain_sdk(owner->local));
    const XrXirNativeArtifactView *native=xr_compile_native_artifact_view(owner->artifact);
    const XtcXirLocalToolchainView *local=xtc_xir_local_toolchain_view(owner->local);
    if (!provider || !sdk || !native || !local)
        return probe_fail(owner,XR_CLI_NATIVE_PROBE_INTEGRITY,"PROBE_INPUT_INVALID","provider-facts",0,0);
    if (!probe_work(owner,sizeof(owner->report.version)*2+3*32+2*sizeof(uint64_t)) ||
        !probe_hex(owner,sdk->identity,owner->report.runtime) ||
        !probe_hex(owner,native->id.bytes,owner->report.fingerprint)) return false;
    uint32_t expected=local->msvc.driver==XR_XIR_WINDOWS_CLANG_CL ?
        XR_TOOLCHAIN_BINDING_PROVIDER_CLANG : XR_TOOLCHAIN_BINDING_PROVIDER_MSVC;
    if (native->input.toolchain.provider!=expected)
        return probe_fail(owner,XR_CLI_NATIVE_PROBE_INTEGRITY,"ABI_MISMATCH","provider-binding",0,0);
    memcpy(owner->report.version,provider->compiler.version.file_text,sizeof(owner->report.version));
    memcpy(owner->compiler_digest,provider->compiler.digest,32);memcpy(owner->linker_digest,provider->linker.digest,32);
    memcpy(owner->sdk_identity,sdk->identity,32);owner->compiler_length=provider->compiler.length;owner->linker_length=provider->linker.length;
    owner->report.provider=expected==XR_TOOLCHAIN_BINDING_PROVIDER_CLANG ? "clang" : "msvc";
    owner->report.native_run=probe_execute(owner) ? "ok" : "failed";
    owner->report.ready=!strcmp(owner->report.native_run,"ok");return owner->report.ready;
}
static bool probe_attempt(XrCliNativeProbe *owner,const XrCliNativeProbeRequest *request,XrXirWindowsDriver driver) {
    XtcXirLocalToolchainRequest locate={request->compiler,NULL,NULL,30000,4*1024*1024,1024*1024,driver};
    XtcXirLocalDiagnostic diagnostic={0};XtcXirLocalStatus found=xtc_xir_local_toolchain_open(owner->resources,&locate,&owner->local,&diagnostic);
    if (found!=XTC_XIR_LOCAL_OK) {
        owner->retryable=(found==XTC_XIR_LOCAL_UNRESOLVED && diagnostic.stage!=XTC_XIR_LOCAL_SDK) ||
            (found==XTC_XIR_LOCAL_CHILD_FAILED && diagnostic.stage==XTC_XIR_LOCAL_STARTUP);
        XrCliNativeProbeStatus mapped=found==XTC_XIR_LOCAL_BUDGET ? XR_CLI_NATIVE_PROBE_BUDGET :
            found==XTC_XIR_LOCAL_OUT_OF_MEMORY ? XR_CLI_NATIVE_PROBE_OUT_OF_MEMORY :
            owner->retryable ? XR_CLI_NATIVE_PROBE_UNAVAILABLE : XR_CLI_NATIVE_PROBE_INTEGRITY;
        return probe_fail(owner,mapped,diagnostic.stage==XTC_XIR_LOCAL_SDK ? "SDK_UNAVAILABLE" :
            diagnostic.stage==XTC_XIR_LOCAL_STARTUP ? "PROVIDER_STARTUP_FAILED" : "TOOLCHAIN_NOT_FOUND",
            diagnostic.stage==XTC_XIR_LOCAL_SDK ? "runtime-sdk" :
            diagnostic.stage==XTC_XIR_LOCAL_STARTUP ? "provider-startup" : "discovery",(unsigned)found,diagnostic.code);
    }
    if (!probe_source(owner)) return false;
    const XtcXirLocalToolchainView *local=xtc_xir_local_toolchain_view(owner->local);
    XtcXirWorkspaceRequest workspace={local->workspace_parent,{XR_PATH_LIMIT_MAX_PATH,32,65536}};
    XtcXirNativeOperationLimits limits={{{4*1024*1024,XR_PATH_LIMIT_MAX_PATH,4096},UINT64_C(64)<<20,4096},30000,4*1024*1024};
    XtcXirNativeOperationStatus status=xtc_xir_native_operation_new(owner->resources,&workspace,&limits,&owner->operation);
    if (status==XTC_XIR_NATIVE_OK) {
        XtcXirNativeOperationRequest run={owner->projection,xtc_xir_local_toolchain_sdk(owner->local),local->compiler,
            local->linker,local->msvc,local->libraries,local->library_count,NULL,NULL};
        status=xtc_xir_native_operation_run(owner->operation,&run);
    }
    if (status==XTC_XIR_NATIVE_OK) {
        owner->report.c_compile=owner->report.sdk_compile=owner->report.runtime_link="ok";return probe_observe(owner);
    }
    const XtcXirNativeOperationDiagnostic *d=xtc_xir_native_operation_diagnostic(owner->operation);
    owner->retryable=status==XTC_XIR_NATIVE_CHILD_FAILED && d && d->domain==XTC_XIR_NATIVE_INVOCATION &&
        d->invocation.domain==XR_XIR_INVOCATION_PROCESS && d->invocation.pass==XR_XIR_INVOCATION_OBSERVE &&
        d->invocation.stage<=XR_XIR_INVOCATION_LINK && d->invocation.exit_code!=0;
    if (d && d->domain==XTC_XIR_NATIVE_INVOCATION && d->invocation.stage<=XR_XIR_INVOCATION_LINK) {
        owner->report.c_compile=d->invocation.stage ? "ok" : "failed";
        if (d->invocation.stage) owner->report.sdk_compile=d->invocation.stage>1 ? "ok" : "failed";
        if (d->invocation.stage==XR_XIR_INVOCATION_LINK) owner->report.runtime_link="failed";
    }
    XrCliNativeProbeStatus mapped=status==XTC_XIR_NATIVE_BUDGET ? XR_CLI_NATIVE_PROBE_BUDGET :
        status==XTC_XIR_NATIVE_OUT_OF_MEMORY ? XR_CLI_NATIVE_PROBE_OUT_OF_MEMORY :
        owner->retryable ? XR_CLI_NATIVE_PROBE_UNAVAILABLE : XR_CLI_NATIVE_PROBE_INTEGRITY;
    probe_fail(owner,mapped,"COMPILE_PROBE_FAILED","native-transaction",(unsigned)status,d ? d->code : 0);
    if (d) snprintf(owner->report.failure.message,sizeof(owner->report.failure.message),
        "native-transaction failed (status=%u domain=%u code=%d stage=%u pass=%u exit=%d)",
        (unsigned)status,(unsigned)d->invocation.domain,d->code,(unsigned)d->invocation.stage,
        (unsigned)d->invocation.pass,d->invocation.exit_code);
    return false;
}
XR_FUNC XrCliNativeProbeStatus xr_cli_native_probe_new(XrCompileResources *resources,XrCliNativeProbe **output) {
    if (!resources || !output || *output) return XR_CLI_NATIVE_PROBE_INVALID;
    XrCliNativeProbe *owner=NULL;
    XrCompileResourceStatus status=xr_compile_resources_calloc(resources,1,sizeof(*owner),(void **)&owner);
    if (status!=XR_COMPILE_RESOURCE_OK) return probe_resource(status);
    owner->resources=resources;probe_report_reset(owner);*output=owner;return XR_CLI_NATIVE_PROBE_OK;
}
XR_FUNC XrCliNativeProbeStatus xr_cli_native_probe_resolve(XrCompileResources *resources,
    const XrCliNativeProbeOptions *options,XrCliNativeProbeRequest *output) {
    if (!resources || !options || !options->preferences || !options->target || !output ||
        (options->preferences->count && !options->preferences->preferences)) return XR_CLI_NATIVE_PROBE_INVALID;
    const XrCliToolchainConfigView *view=options->preferences;
    const XrCliToolchainPreference *preference=NULL;
    for (size_t i=0;i<view->count;++i) {
        XrCompileResourceStatus work=xr_compile_resources_work(resources,7);
        if (work!=XR_COMPILE_RESOURCE_OK) return probe_resource(work);
        if (!strcmp(view->preferences[i].target,options->target)) { preference=&view->preferences[i];break; }
    }
    XrCliNativeProbeRequest selected={XR_TOOLCHAIN_SELECTOR_AUTO,options->compiler,view->allow_fallback};
    if (options->provider) {
        char error[256]={0};
        if (!xtc_selector_parse(options->provider,&selected.selector,error,sizeof(error))) return XR_CLI_NATIVE_PROBE_INVALID;
    } else if (preference) selected.selector=preference->selector;
    if (!selected.compiler && preference && (selected.selector==preference->selector ||
        (selected.selector==XR_TOOLCHAIN_SELECTOR_AUTO && preference->selector==XR_TOOLCHAIN_SELECTOR_MSVC)))
        selected.compiler=preference->compiler;
    XrCompileResourceStatus work=xr_compile_resources_work(resources,sizeof(selected));
    if (work!=XR_COMPILE_RESOURCE_OK) return probe_resource(work);
    *output=selected;return XR_CLI_NATIVE_PROBE_OK;
}
XR_FUNC XrCliNativeProbeStatus xr_cli_native_probe_run(XrCliNativeProbe *owner,const XrCliNativeProbeRequest *request) {
    if (!owner || !request || owner->ran) return XR_CLI_NATIVE_PROBE_INVALID;
    owner->ran=true;uint64_t started=GetTickCount64();
    if (request->selector!=XR_TOOLCHAIN_SELECTOR_AUTO && request->selector!=XR_TOOLCHAIN_SELECTOR_HOST &&
        request->selector!=XR_TOOLCHAIN_SELECTOR_MSVC && request->selector!=XR_TOOLCHAIN_SELECTOR_CLANG) {
        probe_fail(owner,XR_CLI_NATIVE_PROBE_UNSUPPORTED,"TARGET_UNSUPPORTED","provider",0,0);goto done;
    }
    XrXirWindowsDriver driver=request->selector==XR_TOOLCHAIN_SELECTOR_CLANG ? XR_XIR_WINDOWS_CLANG_CL : XR_XIR_WINDOWS_MSVC;
    bool ready=probe_attempt(owner,request,driver);
    XrCliNativeProbeStatus closed=probe_operation_close(owner);
    if (closed!=XR_CLI_NATIVE_PROBE_OK) { owner->status=closed;goto done; }
    if (!ready && owner->retryable && request->selector==XR_TOOLCHAIN_SELECTOR_AUTO &&
        !request->compiler && request->allow_fallback) {
        if (!probe_work(owner,sizeof(owner->report)*2)) goto done;
        owner->report.previous=owner->report.failure;
        xtc_xir_local_toolchain_free(owner->local);owner->local=NULL;probe_report_reset(owner);
        ready=probe_attempt(owner,request,XR_XIR_WINDOWS_CLANG_CL);
        closed=probe_operation_close(owner);
        if (closed!=XR_CLI_NATIVE_PROBE_OK) owner->status=closed;
        else owner->report.fallback_used=ready;
    }
done:
    if (!owner->operation) probe_source_free(owner);
    owner->report.duration_ms=GetTickCount64()-started;return owner->status;
}
XR_FUNC const XrCliNativeProbeReport *xr_cli_native_probe_report(const XrCliNativeProbe *owner) {
    return owner ? &owner->report : NULL;
}
XR_FUNC const XtcXirLocalToolchain *xr_cli_native_probe_local(const XrCliNativeProbe *owner) {
    return owner ? owner->local : NULL;
}
XR_FUNC XrCliNativeProbeStatus xr_cli_native_probe_check_inputs(const XrCliNativeProbe *owner,
    const XrXirInvocationProviderFacts *provider,const XrXirRuntimeSdkFacts *sdk) {
    if (!owner || !owner->ran || owner->status!=XR_CLI_NATIVE_PROBE_OK || !owner->report.ready || !provider || !sdk)
        return XR_CLI_NATIVE_PROBE_INVALID;
    XrCompileResourceStatus work=xr_compile_resources_work(owner->resources,6+2*sizeof(uint64_t)+3*32);
    if (work!=XR_COMPILE_RESOURCE_OK) return probe_resource(work);
    bool same=(provider->compiler.length==owner->compiler_length) & (provider->linker.length==owner->linker_length);
    same&=!memcmp(provider->compiler.digest,owner->compiler_digest,32);
    same&=!memcmp(provider->linker.digest,owner->linker_digest,32);
    same&=!memcmp(sdk->identity,owner->sdk_identity,32);
    return same ? XR_CLI_NATIVE_PROBE_OK : XR_CLI_NATIVE_PROBE_INTEGRITY;
}
XR_FUNC XrCliNativeProbeStatus xr_cli_native_probe_close(XrCliNativeProbe **output) {
    if (!output) return XR_CLI_NATIVE_PROBE_INVALID;
    XrCliNativeProbe *owner=*output;if (!owner) return XR_CLI_NATIVE_PROBE_OK;
    XrCliNativeProbeStatus closed=probe_operation_close(owner);
    if (closed!=XR_CLI_NATIVE_PROBE_OK) return closed;
    probe_source_free(owner);xtc_xir_local_toolchain_free(owner->local);
    xr_compile_resources_free(owner);*output=NULL;return XR_CLI_NATIVE_PROBE_OK;
}
