/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_build.c - One Source product and admitted output publication
 */
#include "xcli.h"
#include "xcli_canonical_source.h"
#include "xcli_source_paths.h"
#include "../toolchain/xtc_xir_publication.h"
#include "../toolchain/xtc_xir_local_toolchain.h"
#include "../toolchain/xtc_xir_native_admission.h"
#include "../../aot/program/xr_xir_native_projection.h"
#include "../../shared/xr_path_limit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif
#define BUILD_C_LIMIT (UINT64_C(64) << 20)

static int build_source_exit(XrCliCompileSourceStatus status) {
    return status == XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY || status == XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT ?
        XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL;
}
static int build_publication_exit(XtcXirPublicationStatus status) {
    return status == XTC_XIR_PUBLICATION_OUT_OF_MEMORY || status == XTC_XIR_PUBLICATION_INVALID ?
        XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL;
}
static bool build_options_supported(const XrCliInvocation *inv) {
    bool c_only = xr_cli_opt_bool(&inv->options,"c-only");
    for (int i = 0; i < inv->options.count; ++i) {
        if (!inv->options.present[i]) continue;
        const char *name = inv->options.spec[i].long_name;
        if (!strcmp(name,"output") || !strcmp(name,"c-only") || !strcmp(name,"native") || !strcmp(name,"verbose")) continue;
        if (!c_only && !strcmp(name,"cc")) continue;
        if (!c_only && !strcmp(name,"toolchain")) {
            const char *provider = xr_cli_opt_string(&inv->options,name,"");
            if (!strcmp(provider,"auto") || !strcmp(provider,"msvc")) continue;
        }
        const char *expected = !strcmp(name,"profile") ? "hosted" : !strcmp(name,"artifact") ? "executable" :
            !strcmp(name,"c-dialect") ? "c11" : !strcmp(name,"target") ? "native" : NULL;
        if (expected && !strcmp(xr_cli_opt_string(&inv->options,name,""),expected)) continue;
        fprintf(stderr,"XR_BUILD_6001: unsupported Source build option --%s\n",name); return false;
    }
    return true;
}
static void build_source_report(const XrCliCompileSourceDiagnostic *diagnostic) {
    const XrXirSourceDiagnostic *source = &diagnostic->source.source;
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(diagnostic->source.snapshot);
    const char *path = diagnostic->source.source_path;
    if (!path && view && view->modules && source->module < view->module_count) path=view->modules[source->module].path;
    if (path && source->line>0) fprintf(stderr,"%s:%d:%d: error: %s\n",path,source->line,source->column,source->message);
    else fprintf(stderr,"XR_BUILD_6003: source build failed (stage=%u status=%s): %s\n",
        (unsigned)diagnostic->stage,xr_cli_compile_source_status_name(diagnostic->status),source->message);
}
static int build_publication_close(XtcXirPublication **owner) {
    bool had_failure=false;
    XtcXirPublicationDiagnostic last={0};
    for (unsigned attempt=0; attempt<8; ++attempt) {
        if (xtc_xir_publication_close(owner)==XTC_XIR_PUBLICATION_OK) {
            if (had_failure) fprintf(stderr,"XR_BUILD_6005: publication cleanup recovered (published=%u os=%u)\n",
                (unsigned)last.published,last.cleanup_os_error);
            return had_failure ? build_publication_exit(last.status) : XR_CLI_EXIT_OK;
        }
        had_failure=true; last=*xtc_xir_publication_diagnostic(*owner);
    }
    fprintf(stderr,"XR_BUILD_6005: terminal publication cleanup failure (published=%u cleanup_pending=%u os=%u)\n",
        (unsigned)last.published,(unsigned)last.cleanup_pending,last.cleanup_os_error);
    fflush(stdout); fflush(stderr);
    /* A one-shot command cannot return and discard an outstanding OS owner.
     * Process termination is explicit here; the publication API never exits. */
    _Exit(XR_CLI_EXIT_INTERNAL);
}
#if defined(XR_ARCH_X86_64) && XR_OS_WINDOWS
#include "xcmd_build_native.inc.c"
#endif
XR_FUNC int cmd_build(const XrCliInvocation *inv) {
    if (!inv || (inv->options.count && (!inv->options.spec || !inv->options.present))) return XR_CLI_EXIT_INTERNAL;
#if XR_OS_WINDOWS
    if (_setmode(_fileno(stdout),_O_BINARY)==-1 || _setmode(_fileno(stderr),_O_BINARY)==-1) return XR_CLI_EXIT_INTERNAL;
#endif
    if (inv->positional_count!=1 || inv->passthrough_argc) {
        xr_cli_error("build","exactly one source file and no passthrough arguments are required"); return XR_CLI_EXIT_USAGE;
    }
    if (!inv->positionals || !inv->positionals[0]) return XR_CLI_EXIT_INTERNAL;
    const char *input=inv->positionals[0];
    size_t spelling_length=strlen(input);
    if (spelling_length<=3 || strcmp(input+spelling_length-3,".xr")) {
        fprintf(stderr,"XR_BUILD_6001: Source build requires an exact .xr source file\n"); return XR_CLI_EXIT_FAIL;
    }
    if (!build_options_supported(inv)) return XR_CLI_EXIT_FAIL;
#if !defined(XR_ARCH_X86_64) || !XR_OS_WINDOWS
    fprintf(stderr,"XR_BUILD_6001: this Source publication target is unsupported\n"); return XR_CLI_EXIT_FAIL;
#else
    bool c_only=xr_cli_opt_bool(&inv->options,"c-only");
    const char *output=xr_cli_opt_string(&inv->options,"output",c_only ? "app.c" : "app.exe");
    XrCompileResourceLimits limits=xr_cli_compile_default_resource_limits();
    XrCompileResources *resources=NULL;
    XrCompileResourceStatus opened=xr_compile_resources_new(&limits,&resources);
    if (opened!=XR_COMPILE_RESOURCE_OK) return opened==XR_COMPILE_RESOURCE_BUDGET ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_INTERNAL;
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCliSourcePaths paths={0}; XrCliSourcePathsDiagnostic path_diagnostic={0};
    XrCliCompileSourceDiagnostic diagnostic={0}; XrXirSourceProduct *product=NULL;
    XrXirNativeProjection *projection=NULL; XtcXirPublication *publication=NULL;
    XrXirNativeArtifact *artifact=NULL;
    int result=XR_CLI_EXIT_FAIL;
    XrCliCompileSourceStatus source=xr_cli_compile_source_paths(resources,input,&paths,&path_diagnostic);
    if (source!=XR_CLI_COMPILE_SOURCE_OK) {
        fprintf(stderr,"XR_BUILD_6003: path lookup failed (stage=%u status=%s io=%u)\n",
            (unsigned)path_diagnostic.stage,xr_cli_compile_source_status_name(source),(unsigned)path_diagnostic.io_status);
        result=build_source_exit(source); goto cleanup;
    }
    XrCliCompileSourceRequest request={&context,paths.entry,paths.stdlib,NULL,
        xr_cli_compile_default_manifest_limits(),{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    source=xr_cli_compile_source_build(&request,&product,&diagnostic);
    if (source!=XR_CLI_COMPILE_SOURCE_OK) { build_source_report(&diagnostic);result=build_source_exit(source);goto cleanup; }
    XrXirNativeProjectionRequest prepare={product,"xray_app",(size_t)BUILD_C_LIMIT};
    XrXirDiagnostic projection_diagnostic={0};
    XrXirStatus projected=xr_compile_native_projection_prepare(&prepare,&projection,&projection_diagnostic);
    if (projected!=XR_XIR_OK) {
        fprintf(stderr,"XR_BUILD_6004: C projection failed (status=%u)\n",(unsigned)projected);
        result=projected==XR_XIR_OUT_OF_MEMORY || projected==XR_XIR_BAD_STRUCTURE ? XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL;
        goto cleanup;
    }
    if (!c_only) {
        result=build_native_prepare(resources,inv,projection,&artifact);
        if (result!=XR_CLI_EXIT_OK) goto cleanup;
    }
    XtcXirPublicationRequest publish={output,{XR_PATH_LIMIT_MAX_PATH,8,BUILD_C_LIMIT}};
    XtcXirPublicationStatus status=xtc_xir_publication_new(resources,&publish,&publication);
    if (status==XTC_XIR_PUBLICATION_OK) {
        if (c_only) {
            const XrXirNativeProjectionSource *c=xr_compile_native_projection_source(projection);
            status=xtc_xir_publication_write(publication,c->text,c->length);
        } else {
            const XrXirNativeArtifactView *native=xr_compile_native_artifact_view(artifact);
            status=xtc_xir_publication_write(publication,native->bytes,native->size);
        }
    }
    xr_compile_native_artifact_free(artifact);artifact=NULL;
    xr_compile_native_projection_owner_free(projection);projection=NULL;
    xr_xir_compile_source_product_free(product);product=NULL;
    xr_cli_compile_source_diagnostic_free(&diagnostic);
    xr_cli_compile_source_paths_free(&paths);
    xr_compile_resources_release(resources);resources=NULL;
    if (status==XTC_XIR_PUBLICATION_OK) status=xtc_xir_publication_commit(publication);
    if (status!=XTC_XIR_PUBLICATION_OK) {
        const XtcXirPublicationDiagnostic *d=xtc_xir_publication_diagnostic(publication);
        fprintf(stderr,"XR_BUILD_6005: output publication failed (status=%u stage=%u published=%u os=%u)\n",
            (unsigned)status,d?(unsigned)d->stage:0,d?(unsigned)d->published:0,d?d->os_error:0);
        result=build_publication_exit(status);
    } else result=XR_CLI_EXIT_OK;
cleanup:
    xr_compile_native_artifact_free(artifact);
    xr_compile_native_projection_owner_free(projection);
    xr_xir_compile_source_product_free(product);
    xr_cli_compile_source_diagnostic_free(&diagnostic);
    xr_cli_compile_source_paths_free(&paths);
    xr_compile_resources_release(resources);
    bool published=publication && xtc_xir_publication_diagnostic(publication)->published;
    int closed=build_publication_close(&publication);
    if (closed>result) result=closed;
    bool report_failed=result==XR_CLI_EXIT_OK && printf("%s: %s\n",c_only ? "Generated" : "Built",output)<0;
    if (fflush(stdout)) report_failed=true;
    if (report_failed) {
        fprintf(stderr,"XR_BUILD_6005: stdout reporting failed (published=%u)\n",(unsigned)published);
        if(result<XR_CLI_EXIT_FAIL)result=XR_CLI_EXIT_FAIL;
    }
    if (ferror(stderr) && result<XR_CLI_EXIT_FAIL) result=XR_CLI_EXIT_FAIL;
    return result;
#endif
}
