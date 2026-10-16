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
#include "../../shared/xr_path_limit.h"
#include "../toolchain/xtc_xir_local_toolchain.h"
#include "../toolchain/xtc_xir_native_admission.h"
#include "xcli_native_probe.h"
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
    bool ready, fallback_used;
    XrCliNativeProbeFailure previous;
    char compiler_path[XR_PATH_LIMIT_MAX_PATH+1];
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
    fputs(",\"profile\":",stdout);toolchain_string(xr_cli_opt_string(&inv->options,"profile","hosted"));
    fputs(",\"provider\":",stdout);
    toolchain_string(xr_cli_opt_string(&inv->options,"provider","auto"));
    fputs("},\"selection\":{\"provider\":",stdout);toolchain_string(report->provider);
    fputs(",\"ownership\":\"external\",\"compiler\":",stdout);toolchain_string(report->compiler ? report->compiler : "");
    fputs(",\"version\":",stdout);toolchain_string(report->version);
    fputs(",\"targetAbi\":",stdout);toolchain_string(report->normalized);
    fputs(",\"runtimeArtifact\":",stdout);toolchain_string(report->runtime);
    printf(",\"ready\":%s,\"fallbackUsed\":%s},\"capabilities\":{\"cCompile\":",
        report->ready ? "true" : "false",report->fallback_used ? "true" : "false");
    toolchain_string(report->c_compile);fputs(",\"sdkCompile\":",stdout);toolchain_string(report->sdk_compile);
    fputs(",\"runtimeLink\":",stdout);toolchain_string(report->runtime_link);
    fputs(",\"nativeRun\":",stdout);toolchain_string(report->native_run);
    fputs(",\"cross\":\"unsupported\",\"lto\":\"unsupported\"},\"diagnostics\":[",stdout);
    if (report->previous.code) {
        fputs("{\"code\":",stdout);toolchain_string(report->previous.code);
        fputs(",\"stage\":",stdout);toolchain_string(report->previous.stage);
        fputs(",\"message\":",stdout);toolchain_string(report->previous.message);putchar('}');
    }
    if (report->code) {
        if (report->previous.code) putchar(',');
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
static bool toolchain_probe_close(XrCliNativeProbe **owner) {
    bool failed=false;
    for (unsigned i=0;i<8;++i) {
        XrCliNativeProbeStatus status=xr_cli_native_probe_close(owner);
        if (status==XR_CLI_NATIVE_PROBE_OK) return !failed;
        if (status!=XR_CLI_NATIVE_PROBE_PENDING) failed=true;
    }
    fputs("toolchain: native probe cleanup remains pending\n",stderr);fflush(stderr);
    _Exit(XR_CLI_EXIT_INTERNAL);
}
static void toolchain_probe_native(XrCompileResources *resources,const XrCliInvocation *inv,
    ToolchainProbeReport *report) {
    uint64_t started=GetTickCount64();XrCliToolchainConfig *preferences=NULL;XrCliNativeProbe *owner=NULL;
    XrCliToolchainConfigStatus configured=xr_cli_toolchain_config_open(resources,NULL,&preferences);
    if (configured!=XR_CLI_TOOLCHAIN_CONFIG_OK) {
        toolchain_probe_failure(report,"CONFIG_INVALID","preferences",(unsigned)configured,0);goto done;
    }
    const XrCliToolchainConfigView *config=xr_cli_toolchain_config_view(preferences);
    if (xr_cli_opt_present(&inv->options,"zig") || strcmp(xr_cli_opt_string(&inv->options,"profile","hosted"),"hosted")) {
        toolchain_probe_failure(report,"TARGET_UNSUPPORTED","provider",0,0);goto done;
    }
    XrCliNativeProbeOptions options={config,report->requested,xr_cli_opt_string(&inv->options,"provider",NULL),
        xr_cli_opt_string(&inv->options,"cc",NULL)};
    XrCliNativeProbeRequest request;
    XrCliNativeProbeStatus resolved=xr_cli_native_probe_resolve(resources,&options,&request);
    if (resolved!=XR_CLI_NATIVE_PROBE_OK) {
        toolchain_probe_failure(report,resolved==XR_CLI_NATIVE_PROBE_BUDGET ? "PROBE_BUDGET" : "PROVIDER_UNSUPPORTED",
            "provider",(unsigned)resolved,0);goto done;
    }
    XrCliNativeProbeStatus status=xr_cli_native_probe_new(resources,&owner);
    if (status==XR_CLI_NATIVE_PROBE_OK) status=xr_cli_native_probe_run(owner,&request);
    const XrCliNativeProbeReport *facts=xr_cli_native_probe_report(owner);
    if (facts) {
        report->provider=facts->provider;report->c_compile=facts->c_compile;report->sdk_compile=facts->sdk_compile;
        report->runtime_link=facts->runtime_link;report->native_run=facts->native_run;
        report->fallback_used=facts->fallback_used;report->previous=facts->previous;
        memcpy(report->version,facts->version,sizeof(report->version));memcpy(report->runtime,facts->runtime,sizeof(report->runtime));
        memcpy(report->fingerprint,facts->fingerprint,sizeof(report->fingerprint));
        report->code=facts->failure.code;report->stage=facts->failure.stage;
        memcpy(report->message,facts->failure.message,sizeof(report->message));
        report->ready=status==XR_CLI_NATIVE_PROBE_OK && facts->ready;
        const XtcXirLocalToolchainView *local=xtc_xir_local_toolchain_view(xr_cli_native_probe_local(owner));
        if (local) {
            int size=snprintf(report->compiler_path,sizeof(report->compiler_path),"%s",local->compiler);
            if (size<0 || (size_t)size>=sizeof(report->compiler_path))
                toolchain_probe_failure(report,"PROBE_INPUT_INVALID","compiler-path",0,0);
            else report->compiler=report->compiler_path;
        }
    }
    if (status!=XR_CLI_NATIVE_PROBE_OK && !report->code)
        toolchain_probe_failure(report,"PROBE_UNAVAILABLE","capability",(unsigned)status,0);
done:
    xr_cli_toolchain_config_free(preferences);
    if (!toolchain_probe_close(&owner)) toolchain_probe_failure(report,"PROBE_CLEANUP_FAILED","cleanup",0,0);
    report->duration_ms=GetTickCount64()-started;toolchain_probe_report(inv,report);
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
