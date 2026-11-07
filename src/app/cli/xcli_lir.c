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
#include "xcli.h"
#include "api/xsource_authority.h"
#include "api/xsource_paths.h"
#include <stdio.h>

static bool report_frontend(const XrXirSourceFailure *failure) {
    if (!failure->report) return false;
    char *text = NULL;
    if (xr_frontend_report_render(failure->report, &text) != XR_COMPILE_RESOURCE_OK)
        return false;
    fputs(text, stderr);
    xr_compile_resources_free(text);
    return true;
}

static void report_lowering(const XrLirSourceFailure *failure, const char *path) {
    if (failure->stage == XR_LIR_SOURCE_CHECK_FAILED) {
        if (!report_frontend(&failure->source_failure))
            fprintf(stderr, "%s:%d:%d: %s\n",
                failure->source_failure.path ? failure->source_failure.path : path,
                failure->source.line, failure->source.column, failure->source.message);
    } else if (failure->stage == XR_LIR_SOURCE_LOWER_FAILED) {
        fprintf(stderr, "%s:%u:%u: %s\n",
            failure->lower.owned_path ? failure->lower.owned_path : path,
            failure->lower.diagnostic.source.line, failure->lower.diagnostic.source.column,
            failure->lower.diagnostic.message);
    } else if (failure->stage == XR_LIR_SOURCE_SPECIALIZATION_FAILED) {
        fprintf(stderr, "source specialization failed (%u)\n", (unsigned)failure->specialization_status);
    } else {
        fprintf(stderr, "cannot compile source to LIR (stage=%u)\n", (unsigned)failure->stage);
    }
}

XR_FUNC int xr_cli_lir_compile(const char *input, XrCompileResources **output_resources,
    XrLirLowered *output) {
    if (!input || !output_resources || *output_resources || !output || output->module ||
        output->entry.module || output->entry.id)
        return XR_CLI_EXIT_INTERNAL;
    XrCompileResourceLimits limits = xr_source_compile_default_resource_limits();
    XrCompileResources *resources = NULL;
    if (xr_compile_resources_new(&limits, &resources) != XR_COMPILE_RESOURCE_OK) {
        fputs("cannot initialize compiler memory\n", stderr);
        return XR_CLI_EXIT_INTERNAL;
    }
    XrSourcePaths paths = {0};
    XrSourcePathsDiagnostic path_diagnostic = {0};
    XrSourceAuthority *authority = NULL;
    XrLirSourceFailure failure = {0};
    XrManifestDiagnostic manifest = {0};
    int result = XR_CLI_EXIT_FAIL;
    if (xr_source_paths(resources, input, &paths, &path_diagnostic) != XR_SOURCE_COMPILE_OK) {
        char text[4096];
        xr_source_paths_diagnostic_format(&path_diagnostic, text, sizeof(text));
        fprintf(stderr, "%s\n", text);
        goto cleanup;
    }
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    XrTomlParseLimits manifest_limits = xr_source_compile_default_manifest_limits();
    XrSourceEntryInput entry = {XR_SOURCE_ENTRY_FILE, paths.entry, NULL, 0};
    XrSourceAuthorityDiagnostics diagnostics = {&manifest, &failure.source, &failure.source_failure};
    XrManifestStatus status = xr_source_authority_open_input(&context, &entry, NULL,
        &manifest_limits, &authority, &diagnostics);
    if (status != XR_MANIFEST_OK) {
        if (!report_frontend(&failure.source_failure))
            fprintf(stderr, "%s: source authority failed (%u): %s\n", paths.entry,
                (unsigned)status, manifest.message);
        goto cleanup;
    }
    XrLirSourceRequest request = {resources, paths.entry, paths.stdlib, false,
        xr_source_authority_entry(authority), xr_source_authority_lockfile(authority)};
    if (xr_lir_compile_source(&request, output, &failure) != XR_LIR_SOURCE_OK) {
        report_lowering(&failure, paths.entry);
        goto cleanup;
    }
    *output_resources = resources;
    resources = NULL;
    result = XR_CLI_EXIT_OK;
cleanup:
    xr_lir_source_failure_free(&failure);
    xr_source_authority_close(authority);
    xr_source_paths_free(&paths);
    xr_source_paths_diagnostic_free(&path_diagnostic);
    xr_compile_resources_release(resources);
    return result;
}
