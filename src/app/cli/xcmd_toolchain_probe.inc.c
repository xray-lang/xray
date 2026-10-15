/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_toolchain_probe.inc.c - Fresh Source and runtime SDK readiness observations
 */
#include "../../api/xsource_compiler.h"
#include "../../api/xsource_paths.h"
#include "../toolchain/xtc_xir_local_toolchain.h"
#include "../toolchain/xtc_xir_native_admission.h"
#include "xray_version.h"
#include "xray_build_identity.h"
#if XR_OS_WINDOWS
#include <windows.h>
#endif

typedef struct ToolchainProbeReport {
    const char *requested, *normalized, *provider, *compiler;
    const char *c_compile, *sdk_compile, *runtime_link, *native_run;
    const char *code, *stage;
    char message[256], version[24], runtime[65], fingerprint[65];
    bool ready;
    uint64_t duration_ms;
} ToolchainProbeReport;
static void toolchain_probe_report(const XrCliInvocation *inv,const ToolchainProbeReport *report) {
    if (!toolchain_json(inv)) {
        printf("AOT toolchain: %s\n",report->ready ? "READY" : "UNAVAILABLE");
        printf("Provider: %s\nCompiler: %s\n",report->provider,report->compiler ? report->compiler : "");
        if (report->code) printf("%s: %s\n",report->stage,report->message);
        return;
    }
    fputs("{\"schema\":1,\"xray\":{\"version\":",stdout);toolchain_string(XRAY_VERSION_STRING);
    fputs(",\"build\":",stdout);toolchain_string(XRAY_BUILD_COMMIT);
    fputs(",\"sdkDigest\":",stdout);toolchain_string(report->runtime);
    fputs("},\"request\":{\"target\":",stdout);toolchain_string(report->requested);
    fputs(",\"normalizedTarget\":",stdout);toolchain_string(report->normalized);
    fputs(",\"profile\":\"hosted\",\"provider\":",stdout);
    toolchain_string(xr_cli_opt_string(&inv->options,"provider","auto"));
    fputs("},\"selection\":{\"provider\":",stdout);toolchain_string(report->provider);
    fputs(",\"ownership\":\"external\",\"compiler\":",stdout);toolchain_string(report->compiler ? report->compiler : "");
    fputs(",\"version\":",stdout);toolchain_string(report->version);
    fputs(",\"targetAbi\":",stdout);toolchain_string(report->normalized);
    fputs(",\"runtimeArtifact\":",stdout);toolchain_string(report->runtime);
    printf(",\"ready\":%s,\"fallbackUsed\":false},\"capabilities\":{\"cCompile\":",report->ready ? "true" : "false");
    toolchain_string(report->c_compile);fputs(",\"sdkCompile\":",stdout);toolchain_string(report->sdk_compile);
    fputs(",\"runtimeLink\":",stdout);toolchain_string(report->runtime_link);
    fputs(",\"nativeRun\":",stdout);toolchain_string(report->native_run);
    fputs(",\"cross\":\"unsupported\",\"lto\":\"unsupported\"},\"diagnostics\":[",stdout);
    if (report->code) {
        fputs("{\"code\":",stdout);toolchain_string(report->code);
        fputs(",\"stage\":",stdout);toolchain_string(report->stage);
        fputs(",\"message\":",stdout);toolchain_string(report->message);putchar('}');
    }
    fputs("],\"probe\":{\"id\":\"xir-source-sdk-native-v1\",\"fingerprint\":",stdout);
    toolchain_string(report->fingerprint);
    printf(",\"cache\":\"disabled\",\"durationMs\":%llu}}\n",(unsigned long long)report->duration_ms);
}
static void toolchain_probe_failure(ToolchainProbeReport *report,const char *code,
    const char *stage,unsigned status,int detail) {
    if (report->code) return;
    report->code=code;report->stage=stage;report->ready=false;
    snprintf(report->message,sizeof(report->message),"%s failed (status=%u detail=%d)",stage,status,detail);
}
#if XR_OS_WINDOWS && defined(XR_ARCH_X86_64)
static bool toolchain_probe_hex(XrCompileResources *resources,const uint8_t bytes[32],char output[65]) {
    if (xr_compile_resources_work(resources,161)!=XR_COMPILE_RESOURCE_OK) return false;
    static const char hex[]="0123456789abcdef";
    for (unsigned i=0;i<32;++i) { output[i*2]=hex[bytes[i]>>4];output[i*2+1]=hex[bytes[i]&15]; }
    output[64]=0;return true;
}
static bool toolchain_probe_close(XtcXirNativeOperation **operation) {
    bool failed=false;
    for (unsigned i=0;i<8;++i) {
        XtcXirNativeOperationStatus status=xtc_xir_native_operation_close(operation,1024);
        if (status==XTC_XIR_NATIVE_OK) return !failed;
        if (status!=XTC_XIR_NATIVE_PENDING) failed=true;
    }
    fputs("toolchain: native probe cleanup remains pending\n",stderr);fflush(stderr);
    /* Do not return while discarding the private workspace and its leases. */
    _Exit(XR_CLI_EXIT_INTERNAL);
}
static bool toolchain_probe_execute(XrCompileResources *resources,XtcXirNativeOperation *operation,
    ToolchainProbeReport *report) {
    const XrXirInvocationFacts *facts=xtc_xir_native_operation_facts(operation);
    const XrProcessView *link=xtc_xir_native_operation_command(operation,XR_XIR_INVOCATION_LINK);
    const XrXirInvocationFile *output=NULL;
    if (!facts || facts->completed_runs!=6 || !link || link->env_count>XTC_PROCESS_MAX_ENV) return false;
    for (uint32_t i=0;i<facts->file_count;++i) {
        if (xr_compile_resources_work(resources,1)!=XR_COMPILE_RESOURCE_OK) return false;
        const XrXirInvocationFile *file=xtc_xir_native_operation_file(operation,i);
        if (file && file->kind==XR_XIR_INVOCATION_OUTPUT) {
            if (output) return false;
            output=file;
        }
    }
    if (!output) return false;
    XrProcessSpec spec;xtc_process_spec_init(&spec,output->path,30000);
    spec.environment_source=XTC_PROCESS_ENV_EXPLICIT;spec.cwd=link->cwd;spec.output_limit=4096;
    spec.env_count=link->env_count;
    for (size_t i=0;i<link->env_count;++i) { spec.env_keys[i]=link->env_keys[i];spec.env_values[i]=link->env_values[i]; }
    XrToolchainProcess *process=NULL;XrProcessResult result={0};
    XrProcessStatus status=xtc_process_prepare(resources,&spec,&process);
    if (status==XTC_PROCESS_OK) status=xtc_process_run(process,NULL,NULL,&result);
    bool okay=status==XTC_PROCESS_OK && result.exit_code==0 &&
        !result.stdout_bytes.truncated && !result.stderr_bytes.truncated &&
        result.stdout_bytes.length==3 && !memcmp(result.stdout_bytes.data,"42\n",3) && !result.stderr_bytes.length;
    if (!okay) toolchain_probe_failure(report,"RUN_PROBE_FAILED","native-run",(unsigned)status,result.exit_code);
    xtc_process_result_free(&result);xtc_process_free(process);return okay;
}
static void toolchain_probe_native(XrCompileResources *resources,const XrCliInvocation *inv,
    ToolchainProbeReport *report) {
    uint64_t started=GetTickCount64();XrCliToolchainConfig *preferences=NULL;XtcXirLocalToolchain *local=NULL;
    XrSourceCompiler *compiler=NULL;XrXirSourceProduct *product=NULL;XrXirNativeProjection *projection=NULL;
    XrXirNativeArtifact *artifact=NULL;XtcXirNativeOperation *operation=NULL;
    XrSourceCompileDiagnostic diagnostic={0};XrStdlibPath stdlib={0};XrSourcePathsDiagnostic path={0};
    XrToolchainSelector selector=XR_TOOLCHAIN_SELECTOR_AUTO;char error[256]={0};
    XrCliToolchainConfigStatus configured=xr_cli_toolchain_config_open(resources,NULL,&preferences);
    if (configured!=XR_CLI_TOOLCHAIN_CONFIG_OK) {
        toolchain_probe_failure(report,"CONFIG_INVALID","preferences",(unsigned)configured,0);goto done;
    }
    const XrCliToolchainConfigView *config=xr_cli_toolchain_config_view(preferences);
    const XrCliToolchainPreference *preference=NULL;
    for (size_t i=0;i<config->count;++i) if (!strcmp(config->preferences[i].target,report->requested)) preference=&config->preferences[i];
    const char *requested=xr_cli_opt_string(&inv->options,"provider",NULL);
    if (requested) {
        if (!xtc_selector_parse(requested,&selector,error,sizeof(error))) {
            toolchain_probe_failure(report,"PROVIDER_UNSUPPORTED","provider",0,0);goto done;
        }
    } else if (preference) selector=preference->selector;
    if ((selector!=XR_TOOLCHAIN_SELECTOR_AUTO && selector!=XR_TOOLCHAIN_SELECTOR_HOST && selector!=XR_TOOLCHAIN_SELECTOR_MSVC) ||
        xr_cli_opt_present(&inv->options,"zig") || strcmp(xr_cli_opt_string(&inv->options,"profile","hosted"),"hosted")) {
        toolchain_probe_failure(report,"TARGET_UNSUPPORTED","provider",0,0);goto done;
    }
    const char *cc=xr_cli_opt_string(&inv->options,"cc",NULL);
    if (!cc && preference && (selector==preference->selector ||
        (selector==XR_TOOLCHAIN_SELECTOR_AUTO && preference->selector==XR_TOOLCHAIN_SELECTOR_MSVC))) cc=preference->compiler;
    XtcXirLocalToolchainRequest locate={cc,NULL,NULL,30000,4*1024*1024,1024*1024};
    XtcXirLocalDiagnostic found={0};XtcXirLocalStatus located=xtc_xir_local_toolchain_open(resources,&locate,&local,&found);
    if (located!=XTC_XIR_LOCAL_OK) {
        toolchain_probe_failure(report,found.stage==XTC_XIR_LOCAL_SDK ? "SDK_UNAVAILABLE" : "TOOLCHAIN_NOT_FOUND",
            found.stage==XTC_XIR_LOCAL_SDK ? "runtime-sdk" : "discovery",(unsigned)located,found.code);goto done;
    }
    const XtcXirLocalToolchainView *view=xtc_xir_local_toolchain_view(local);
    report->compiler=view->compiler;
    XrSourceCompileStatus source=xr_source_stdlib_path(resources,&stdlib,&path);
    XrSourceCompilerConfig source_config={{resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},stdlib.path,xr_source_compile_default_manifest_limits()};
    if (source==XR_SOURCE_COMPILE_OK) source=xr_source_compiler_new(&source_config,&compiler,&diagnostic);
    const XrSourceCompilerText text={NULL,"xray-toolchain-probe","print(42)\n",10};
    if (source==XR_SOURCE_COMPILE_OK) source=xr_source_compiler_build_text(compiler,&text,&product,&diagnostic);
    if (source!=XR_SOURCE_COMPILE_OK) {
        toolchain_probe_failure(report,"COMPILE_PROBE_FAILED","source-program",(unsigned)source,(int)diagnostic.stage);goto done;
    }
    XrXirNativeProjectionRequest project={product,"xray_toolchain_probe",UINT64_C(64)<<20};
    XrXirStatus projected=xr_compile_native_projection_prepare(&project,&projection,NULL);
    if (projected!=XR_XIR_OK) {
        toolchain_probe_failure(report,"COMPILE_PROBE_FAILED","native-projection",(unsigned)projected,0);goto done;
    }
    XtcXirWorkspaceRequest workspace={view->workspace_parent,{XR_PATH_LIMIT_MAX_PATH,32,65536}};
    XtcXirNativeOperationLimits limits={{{4*1024*1024,XR_PATH_LIMIT_MAX_PATH,4096},UINT64_C(64)<<20,4096},30000,4*1024*1024};
    XtcXirNativeOperationStatus status=xtc_xir_native_operation_new(resources,&workspace,&limits,&operation);
    if (status==XTC_XIR_NATIVE_OK) {
        XtcXirNativeOperationRequest request={projection,xtc_xir_local_toolchain_sdk(local),view->compiler,view->linker,
            view->msvc,view->libraries,view->library_count,NULL,NULL};
        status=xtc_xir_native_operation_run(operation,&request);
    }
    if (status!=XTC_XIR_NATIVE_OK) {
        const XtcXirNativeOperationDiagnostic *d=xtc_xir_native_operation_diagnostic(operation);
        unsigned stage=d ? (unsigned)d->invocation.stage : 3;
        if (d && d->domain==XTC_XIR_NATIVE_INVOCATION && stage<3) {
            report->c_compile=stage>0 ? "ok" : "failed";
            if (stage>0) report->sdk_compile=stage>1 ? "ok" : "failed";
            if (stage==2) report->runtime_link="failed";
        }
        toolchain_probe_failure(report,"COMPILE_PROBE_FAILED","native-transaction",(unsigned)status,d ? d->code : 0);goto done;
    }
    report->c_compile=report->sdk_compile=report->runtime_link="ok";
    XtcXirNativeAdmissionDiagnostic admitted={0};
    XtcXirNativeAdmissionStatus admission=xtc_xir_native_admit(operation,projection,
        xtc_xir_local_toolchain_sdk(local),UINT64_C(64)<<20,&artifact,&admitted);
    if (admission!=XTC_XIR_ADMISSION_OK) {
        toolchain_probe_failure(report,"ABI_MISMATCH","native-admission",(unsigned)admission,admitted.code);goto done;
    }
    const XrXirInvocationProviderFacts *provider=xtc_xir_native_operation_provider(operation);
    const XrXirRuntimeSdkFacts *sdk=xr_xir_runtime_sdk_facts(xtc_xir_local_toolchain_sdk(local));
    const XrXirNativeArtifactView *native=xr_compile_native_artifact_view(artifact);
    if (!provider || !sdk || !native || xr_compile_resources_work(resources,sizeof(report->version)*2)!=XR_COMPILE_RESOURCE_OK ||
        !toolchain_probe_hex(resources,sdk->identity,report->runtime) || !toolchain_probe_hex(resources,native->id.bytes,report->fingerprint)) {
        toolchain_probe_failure(report,"PROBE_BUDGET","provider-facts",0,0);goto done;
    }
    memcpy(report->version,provider->compiler.version.file_text,sizeof(report->version));report->provider="msvc";
    report->native_run=toolchain_probe_execute(resources,operation,report) ? "ok" : "failed";
    if (!strcmp(report->native_run,"failed") && !report->code)
        toolchain_probe_failure(report,"PROBE_INPUT_INVALID","native-run-input",0,0);
    report->ready=!strcmp(report->native_run,"ok");
done:
    xr_compile_native_artifact_free(artifact);xr_compile_native_projection_owner_free(projection);
    xr_xir_compile_source_product_free(product);xr_source_compiler_free(compiler);
    xr_source_compile_diagnostic_free(&diagnostic);xr_source_stdlib_path_free(&stdlib);
    xr_cli_toolchain_config_free(preferences);
    if (!toolchain_probe_close(&operation)) toolchain_probe_failure(report,"PROBE_CLEANUP_FAILED","cleanup",0,0);
    report->duration_ms=GetTickCount64()-started;
    toolchain_probe_report(inv,report);xtc_xir_local_toolchain_free(local);
}
#endif
static int toolchain_probe(XrCompileResources *resources,const XrCliInvocation *inv) {
    if (inv->positional_count!=1 || inv->passthrough_argc) return XR_CLI_EXIT_USAGE;
    ToolchainProbeReport report={0};report.requested=xr_cli_opt_string(&inv->options,"target","native");
    XrToolchainTarget target={0};char error[256]={0};
    if (!xtc_target_parse(report.requested,&target,error,sizeof(error))) return XR_CLI_EXIT_USAGE;
    report.normalized=target.name;report.provider="none";
    report.c_compile=report.sdk_compile=report.runtime_link=report.native_run="skipped";
#if XR_OS_WINDOWS && defined(XR_ARCH_X86_64)
    if (target.is_native) toolchain_probe_native(resources,inv,&report);
    else {
        toolchain_probe_failure(&report,"TARGET_UNSUPPORTED","target",0,0);toolchain_probe_report(inv,&report);
    }
#else
    (void)resources;
    toolchain_probe_failure(&report,"TARGET_UNSUPPORTED","host",0,0);toolchain_probe_report(inv,&report);
#endif
    return report.ready ? XR_CLI_EXIT_OK : XR_CLI_EXIT_UNAVAILABLE;
}
