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
 *   One compiler ledger builds the ordinary Source product. Its transferred VM
 *   Program outlives every temporary producer and uses the shared host policy.
 */
#include "xcli.h"
#include "xcli_canonical_source.h"
#include "xcli_source_paths.h"
#include "../../execution/xr_xir_host_cli.h"
#include <stdio.h>
#include <string.h>
#if XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

/* This checks the command's argument spelling. Owned path lookup and UTF
 * conversion begin only after the compiler ledger has been created. */
static bool is_exact_source_path(const char *path) {
    size_t length = path ? strlen(path) : 0;
    return length > 3 && strcmp(path + length - 3, ".xr") == 0;
}

#if defined(XR_ARCH_X86_64)
static int source_exit(XrCliCompileSourceStatus status) {
    return status == XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY ||
        status == XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT ? XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL;
}

static int report_source_failure(const XrCliCompileSourceDiagnostic *diagnostic) {
    const XrXirSourceDiagnostic *source = &diagnostic->source.source;
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(diagnostic->source.snapshot);
    const char *path = diagnostic->source.source_path;
    if (!path && view && view->modules && source->module < view->module_count)
        path = view->modules[source->module].path;
    if (path && path[0] && source->line > 0) {
        fprintf(stderr, "%s:%d:%d: error: %s\n", path, source->line, source->column,
            source->message[0] ? source->message : xr_cli_compile_source_status_name(diagnostic->status));
    } else {
        fprintf(stderr, "XR_RUN_6001: source build failed (stage=%u status=%s)",
            (unsigned)diagnostic->stage, xr_cli_compile_source_status_name(diagnostic->status));
        if (source->message[0]) fprintf(stderr, ": %s", source->message);
        fputc('\n', stderr);
    }
    return source_exit(diagnostic->status);
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
    XrCompileResourceLimits limits = xr_cli_compile_default_resource_limits();
    XrCompileResources *resources = NULL;
    XrCompileResourceStatus opened = xr_compile_resources_new(&limits, &resources);
    if (opened != XR_COMPILE_RESOURCE_OK) {
        fprintf(stderr, "XR_RUN_6001: cannot initialize source resources\n");
        return opened == XR_COMPILE_RESOURCE_BUDGET ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_INTERNAL;
    }
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    XrCliSourcePaths paths = {0};
    XrCliSourcePathsDiagnostic path_diagnostic = {0};
    XrCliCompileSourceStatus status = xr_cli_compile_source_paths(resources, source_path,
        &paths, &path_diagnostic);
    XrXirSourceProduct *product = NULL;
    XrCliCompileSourceDiagnostic diagnostic = {0};
    XrXirProgram *program = NULL;
    uint32_t entry = 0;
    int result = XR_CLI_EXIT_FAIL;
    if (status != XR_CLI_COMPILE_SOURCE_OK) {
        fprintf(stderr, "XR_RUN_6001: source path lookup failed (stage=%u status=%s io=%u)\n",
            (unsigned)path_diagnostic.stage, xr_cli_compile_source_status_name(status),
            (unsigned)path_diagnostic.io_status);
        result = source_exit(status);
    } else {
        XrCliCompileSourceRequest request = {&context, paths.entry, paths.stdlib, NULL,
            xr_cli_compile_default_manifest_limits(), {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
        status = xr_cli_compile_source_build(&request, &product, &diagnostic);
        if (status != XR_CLI_COMPILE_SOURCE_OK) {
            result = report_source_failure(&diagnostic);
        } else {
            const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(product);
            if (!facts) {
                fprintf(stderr, "XR_RUN_6002: source product has no admitted entry\n");
                result = XR_CLI_EXIT_INTERNAL;
            } else {
                entry = facts->entry;
                XrXirStatus taken = xr_xir_compile_source_product_vm_take(product, &program);
                if (taken != XR_XIR_OK) {
                    fprintf(stderr, "XR_RUN_6002: cannot prepare VM program (status=%u)\n", (unsigned)taken);
                    result = taken == XR_XIR_BUDGET ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_INTERNAL;
                }
            }
        }
    }
    xr_cli_compile_source_paths_free(&paths);
    xr_xir_compile_source_product_free(product);
    xr_cli_compile_source_diagnostic_free(&diagnostic);
    xr_compile_resources_release(resources);
    return program ? xr_xir_host_program_main(program, entry) : result;
#endif
}
