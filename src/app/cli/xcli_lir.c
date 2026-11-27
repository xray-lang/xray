/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_lir.c - Project authority and owned LIR at command boundaries
 */
#include "xcli_lir.h"
#if defined(XR_LIR_CLI_HAS_MATH_ARCHIVE)
#include "xir/xxir_builtin_stdlib.h"
#endif
#include "xcli.h"
#include "api/xsource_authority.h"
#include "api/xsource_paths.h"
#include <stdarg.h>
#include <stdio.h>

static void cli_compile_diagnostic(bool quiet, XrCliTestCompileFailure *cause, XrSourceCompileStatus status,
    XrSourceCompileStage stage, const XrFrontendReport *report, const char *format, ...) XR_PRINTF_FMT(6, 7);
static void cli_compile_diagnostic(bool quiet, XrCliTestCompileFailure *cause, XrSourceCompileStatus status,
    XrSourceCompileStage stage, const XrFrontendReport *report, const char *format, ...) {
    if (quiet && !cause) return;
    if (cause) {
        cause->status = status;
        cause->stage = stage;
    }
    char *rendered = NULL;
    if (report && xr_frontend_report_render(report, &rendered) == XR_COMPILE_RESOURCE_OK) {
        if (!quiet) fputs(rendered, stderr);
        if (cause) cause->text = rendered;
        else xr_compile_resources_free(rendered);
        return;
    }

    va_list args, copy;
    va_start(args, format);
    if (!quiet) {
        va_copy(copy, args);
        vfprintf(stderr, format, copy);
        va_end(copy);
    }
    /* Do not substitute a shortened diagnostic when an existing report cannot be rendered. */
    if (cause && !report) {
        va_copy(copy, args);
        int length = vsnprintf(NULL, 0, format, copy);
        va_end(copy);
        if (length >= 0 && xr_compile_resources_alloc(cause->resources, (size_t)length + 1,
                (void **)&cause->text) == XR_COMPILE_RESOURCE_OK &&
            vsnprintf(cause->text, (size_t)length + 1, format, args) != length) {
            xr_compile_resources_free(cause->text);
            cause->text = NULL;
        }
    }
    va_end(args);
}

static void report_lowering(bool quiet, XrCliTestCompileFailure *cause, const XrLirSourceFailure *failure,
    const char *path) {
    if (failure->stage == XR_LIR_SOURCE_CHECK_FAILED) {
        cli_compile_diagnostic(quiet, cause,
            xr_source_compile_from_xir(failure->source.status, failure->source_failure.origin),
            XR_SOURCE_COMPILE_CHECK, failure->source_failure.report, "%s:%d:%d: %s\n",
            failure->source_failure.path ? failure->source_failure.path : path,
            failure->source.line, failure->source.column, failure->source.message);
    } else if (failure->stage == XR_LIR_SOURCE_LOWER_FAILED) {
        cli_compile_diagnostic(quiet, cause, XR_SOURCE_COMPILE_OK, XR_SOURCE_COMPILE_CHECK, NULL, "%s:%u:%u: %s\n",
            failure->lower.owned_path ? failure->lower.owned_path : path,
            failure->lower.diagnostic.source.line, failure->lower.diagnostic.source.column,
            failure->lower.diagnostic.message);
    } else if (failure->stage == XR_LIR_SOURCE_SPECIALIZATION_FAILED) {
        cli_compile_diagnostic(quiet, cause, XR_SOURCE_COMPILE_OK, XR_SOURCE_COMPILE_CHECK, NULL,
            "source specialization failed (%u)\n", (unsigned)failure->specialization_status);
    } else {
        cli_compile_diagnostic(quiet, cause, XR_SOURCE_COMPILE_OK, XR_SOURCE_COMPILE_CHECK, NULL,
            "cannot compile source to LIR (stage=%u)\n", (unsigned)failure->stage);
    }
}

static int cli_lir_compile(const char *input, XrCompileResources **output_resources, XrLirLowered *output,
    const char *filter, bool quiet, XrLirTestPlan **plan, XrCliTestCompileFailure *cause) {
    XrCompileResourceLimits limits = xr_source_compile_default_resource_limits();
    XrCompileResources *resources = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_new(&limits, &resources);
    if (allocated != XR_COMPILE_RESOURCE_OK) {
        cli_compile_diagnostic(quiet, cause, xr_source_compile_from_resource(allocated), XR_SOURCE_COMPILE_SESSION,
            NULL, "cannot initialize compiler memory\n");
        return XR_CLI_EXIT_INTERNAL;
    }
    XrSourcePaths paths = {0};
    XrSourcePathsDiagnostic path_diagnostic = {0};
    XrSourceAuthority *authority = NULL;
    XrLirSourceFailure failure = {0};
    XrManifestDiagnostic manifest = {0};
    int result = XR_CLI_EXIT_FAIL;
    XrSourceCompileStatus path_status = xr_source_paths(resources, input, &paths, &path_diagnostic);
    if (path_status != XR_SOURCE_COMPILE_OK) {
        if (!quiet || cause) {
            char text[4096];
            xr_source_paths_diagnostic_format(&path_diagnostic, text, sizeof(text));
            cli_compile_diagnostic(quiet, cause, path_status, XR_SOURCE_COMPILE_PATH, NULL, "%s\n", text);
        }
        goto cleanup;
    }
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    XrTomlParseLimits manifest_limits = xr_source_compile_default_manifest_limits();
    XrSourceEntryInput entry = {XR_SOURCE_ENTRY_FILE, paths.entry, NULL, 0};
    XrSourceAuthorityDiagnostics diagnostics = {&manifest, &failure.source, &failure.source_failure};
    XrManifestStatus status = xr_source_authority_open_input(&context, &entry, NULL,
        &manifest_limits, &authority, &diagnostics);
    if (status != XR_MANIFEST_OK) {
        cli_compile_diagnostic(quiet, cause, xr_source_compile_from_manifest(status), XR_SOURCE_COMPILE_AUTHORITY,
            failure.source_failure.report, "%s: source authority failed (%u): %s\n",
            paths.entry, (unsigned)status, manifest.message);
        goto cleanup;
    }
    XrLirSourceRequest request = {resources, paths.entry, paths.stdlib, false,
        xr_source_authority_entry(authority), xr_source_authority_lockfile(authority), xr_xir_target_host()};
    const XrXirBuiltinStdlibModule *archive = NULL;
#if defined(XR_LIR_CLI_HAS_MATH_ARCHIVE)
    archive = &xray_stdlib_math_interface;
#endif
    XrLirSourceStatus compiled = plan ? xr_lir_compile_test_plan_selected(&request, filter, plan, &failure)
        : xr_lir_compile_source_with_archive(&request, archive, output, &failure);
    if (compiled != XR_LIR_SOURCE_OK) {
        report_lowering(quiet, cause, &failure, paths.entry);
        goto cleanup;
    }
    if (output_resources) {
        *output_resources = resources;
        resources = NULL;
    }
    result = XR_CLI_EXIT_OK;
cleanup:
    xr_lir_source_failure_free(&failure);
    xr_source_authority_close(authority);
    xr_source_paths_free(&paths);
    xr_source_paths_diagnostic_free(&path_diagnostic);
    xr_compile_resources_release(resources);
    return result;
}

XR_FUNC int xr_cli_lir_compile(const char *input, XrCompileResources **output_resources,
    XrLirLowered *output) {
    if (!input || !output_resources || *output_resources || !output || output->module ||
        output->entry.module || output->entry.id)
        return XR_CLI_EXIT_INTERNAL;
    return cli_lir_compile(input, output_resources, output, NULL, false, NULL, NULL);
}

XR_FUNC int xr_cli_lir_compile_test_plan(const char *input, const char *filter, bool quiet,
    XrLirTestPlan **plan, XrCliTestCompileFailure *cause) {
    if (!input || !plan || *plan || (cause && (!cause->resources || cause->text)))
        return XR_CLI_EXIT_INTERNAL;
    return cli_lir_compile(input, NULL, NULL, filter, quiet, plan, cause);
}
