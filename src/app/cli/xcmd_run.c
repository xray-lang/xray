/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_run.c - Source admission and owned Program consumption for run
 *
 * KEY CONCEPT:
 *   One compiler memory owner builds the ordinary Source product. Its transferred VM
 *   Program outlives every temporary producer and uses the shared host policy.
 */
#include "xcli.h"
#include "../../api/xsource_compile.h"
#include "../../api/xsource_paths.h"
#include "../../api/xsource_api.h"
#include "../../base/xresource_limits.h"
#include "../../execution/xr_xir_host_cli.h"
#include <stdio.h>
#include <string.h>
#if XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

/* This checks the command's argument spelling. Owned path lookup and UTF
 * conversion begin only after the compiler memory owner has been created. */
static bool is_exact_source_path(const char *path) {
    size_t length = path ? strlen(path) : 0;
    return length > 3 && strcmp(path + length - 3, ".xr") == 0;
}

#if defined(XR_ARCH_X86_64)
static int source_exit(XrSourceCompileStatus status) {
    return status == XR_SOURCE_COMPILE_OUT_OF_MEMORY ||
        status == XR_SOURCE_COMPILE_BAD_ARGUMENT ? XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL;
}

static int report_source_failure(const XrSourceCompileDiagnostic *diagnostic,XrCompileResources *resources) {
    char *text=NULL;XrSourceCompileStatus shown=xr_source_compile_diagnostic_render(diagnostic,resources,&text);
    if(shown==XR_SOURCE_COMPILE_OK)fputs(text,stderr);
    else fprintf(stderr,"XR_RUN_6001: diagnostic rendering failed (%s)\n",xr_source_compile_status_name(shown));
    xr_compile_resources_free(text);return source_exit(diagnostic->status);
}
#endif

XR_FUNC int cmd_run(const XrCliInvocation *inv) {
    if (!inv) return XR_CLI_EXIT_INTERNAL;
#if XR_OS_WINDOWS
    if (_setmode(_fileno(stdout), _O_BINARY) == -1 ||
        _setmode(_fileno(stderr), _O_BINARY) == -1) return XR_CLI_EXIT_INTERNAL;
#endif
    if (inv->positional_count != 1) {
        xr_cli_error("run", "exactly one source file is required");
        return XR_CLI_EXIT_USAGE;
    }
    if (!inv->positionals) return XR_CLI_EXIT_INTERNAL;
    if (inv->passthrough_argc != 0) {
        fprintf(stderr, "XR_RUN_6010: a module initializer does not accept arguments\n");
        return XR_CLI_EXIT_FAIL;
    }
    const char *source_path = inv->positionals[0];
    if (!source_path || !source_path[0] || (source_path[0] == '-' && !source_path[1])) {
        fprintf(stderr, "XR_RUN_6011: canonical run requires a file-backed source authority\n");
        return XR_CLI_EXIT_FAIL;
    }
    if (!is_exact_source_path(source_path)) {
        fprintf(stderr, "XR_RUN_6013: canonical run accepts only an exact '.xr' source path\n");
        return XR_CLI_EXIT_FAIL;
    }
#if !defined(XR_ARCH_X86_64)
    fprintf(stderr, "XR_RUN_6002: source VM does not support this host architecture\n");
    return XR_CLI_EXIT_FAIL;
#else
    bool sandbox = xr_cli_opt_bool(&inv->options, "sandbox");
    XrCompileResourceLimits limits = sandbox ?
        (XrCompileResourceLimits)XR_RESOURCE_SANDBOX_COMPILER_LIMITS :
        (XrCompileResourceLimits)XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrCompileResources *resources = NULL;
    XrCompileResourceStatus opened = xr_compile_resources_new(&limits, &resources);
    if (opened != XR_COMPILE_RESOURCE_OK) {
        fprintf(stderr, "XR_RUN_6001: cannot initialize source resources\n");
        return opened == XR_COMPILE_RESOURCE_MEMORY_LIMIT || opened == XR_COMPILE_RESOURCE_OVERFLOW ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_INTERNAL;
    }
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    XrSourcePaths paths = {0};
    XrSourcePathsDiagnostic path_diagnostic = {0};
    XrSourceCompileStatus status = xr_source_paths(resources, source_path,
        &paths, &path_diagnostic);
    XrXirSourceProduct *product = NULL;
    XrSourceCompileDiagnostic diagnostic = {0};
    XrSourceArtifact *owned = NULL;
    XrXirProgram *program = NULL;
    uint32_t entry = 0;
    int result = XR_CLI_EXIT_FAIL;
    if (status != XR_SOURCE_COMPILE_OK) {
        char text[4096];
        xr_source_paths_diagnostic_format(&path_diagnostic, text, sizeof(text));
        fprintf(stderr, "XR_RUN_6001: %s\n", text);
        result = source_exit(status);
    } else {
        XrSourceCompileRequest request = {&context, paths.entry, paths.stdlib, NULL,
            xr_source_compile_default_manifest_limits(), xr_xir_host_target(), NULL};
        status = xr_source_compile_build(&request, &product, &diagnostic);
        if (status != XR_SOURCE_COMPILE_OK) {
            result = report_source_failure(&diagnostic,resources);
        } else {
            XrSourceStatus wrapped = xr_source_artifact_take_product(&product, XR_SOURCE_TARGET_VM, &owned);
            if (wrapped != XR_SOURCE_OK) {
                fprintf(stderr, "XR_RUN_6002: cannot admit owned API program (status=%u)\n", (unsigned)wrapped);
                result = wrapped == XR_SOURCE_MEMORY_LIMIT || wrapped == XR_SOURCE_OVERFLOW ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_INTERNAL;
            } else {
                XrSourceStatus taken = xr_source_artifact_take_vm(&owned, &program, &entry);
                if (taken != XR_SOURCE_OK) {
                    fprintf(stderr, "XR_RUN_6002: cannot prepare VM program (status=%u)\n", (unsigned)taken);
                    result = taken == XR_SOURCE_MEMORY_LIMIT || taken == XR_SOURCE_OVERFLOW ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_INTERNAL;
                }
            }
        }
    }
    xr_source_paths_free(&paths);
    xr_source_paths_diagnostic_free(&path_diagnostic);
    xray_source_artifact_free(owned);
    xr_xir_compile_source_product_free(product);
    xr_source_compile_diagnostic_free(&diagnostic);
    xr_compile_resources_release(resources);
    return program ? xr_xir_host_program_main(program, entry, sandbox) : result;
#endif
}
