/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_deps.c - 'xray deps' command implementation
 *
 * KEY CONCEPT:
 *   Analyzes project dependencies and generates install scripts.
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "xcli_canonical_source.h"
#include "xcli_source_paths.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "xcli_dependency_output.h"

XR_FUNC int cmd_deps(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    XR_DCHECK(inv->positional_count == 1, "deps expects exactly 1 positional");

    const char *input_file = inv->positionals[0];
    const char *output_file = xr_cli_opt_string(&inv->options, "output", NULL);

    OutputFormat format = OUTPUT_SHELL;
    if (xr_cli_opt_present(&inv->options, "json"))
        format = OUTPUT_JSON;
    else if (xr_cli_opt_present(&inv->options, "list"))
        format = OUTPUT_LIST;
    else if (xr_cli_opt_present(&inv->options, "shell"))
        format = OUTPUT_SHELL;

    XrCompileResourceLimits limits = xr_cli_compile_default_resource_limits();
    XrCompileResources *resources = NULL;
    if (xr_compile_resources_new(&limits, &resources) != XR_COMPILE_RESOURCE_OK) {
        xr_cli_error("deps", "cannot initialize compiler resources");
        return XR_CLI_EXIT_INTERNAL;
    }
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    XrCliSourcePaths paths = {0}; XrCliSourcePathsDiagnostic path_diagnostic = {0};
    XrCliCompileSourceDiagnostic diagnostic = {0}; XrXirSourceProduct *product = NULL;
    XrCliCompileSourceStatus status = xr_cli_compile_source_paths(resources, input_file,
        &paths, &path_diagnostic);
    if (status == XR_CLI_COMPILE_SOURCE_OK) {
        XrCliCompileSourceRequest request = {&context, paths.entry, paths.stdlib, NULL,
            xr_cli_compile_default_manifest_limits(), {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
        status = xr_cli_compile_source_build(&request, &product, &diagnostic);
    }
    if (status != XR_CLI_COMPILE_SOURCE_OK) {
        char text[1024] = {0};
        if (diagnostic.status != XR_CLI_COMPILE_SOURCE_OK) {
            (void)xr_cli_compile_source_diagnostic_format(&diagnostic, text, sizeof(text));
            xr_cli_error("deps", "%s", text);
        } else xr_cli_error("deps", "source path lookup failed: %s",
            xr_cli_compile_source_status_name(status));
    }
    xr_cli_compile_source_paths_free(&paths);
    xr_cli_compile_source_diagnostic_free(&diagnostic);
    xr_compile_resources_release(resources);
    const XrXirSourceDependencies *bundle = xr_xir_compile_source_product_dependencies(product);
    if (status != XR_CLI_COMPILE_SOURCE_OK || !bundle) {
        xr_cli_error("deps", "dependency analysis failed for '%s'", input_file);
        xr_xir_compile_source_product_free(product);
        return XR_CLI_EXIT_FAIL;
    }

    /* Open output file */
    FILE *out = stdout;
    if (output_file) {
        out = fopen(output_file, "w");
        if (!out) {
            xr_cli_error("deps", "cannot create '%s'", output_file);
            xr_xir_compile_source_product_free(product);
            return XR_CLI_EXIT_FAIL;
        }
    }

    bool output_failed = !deps_emit(out, bundle, format);
    if (output_file) {
        if (fclose(out) != 0) output_failed = true;
        if (output_failed) {
            xr_cli_error("deps", "cannot write '%s'", output_file);
            xr_xir_compile_source_product_free(product);
            return XR_CLI_EXIT_FAIL;
        }
        printf("Dependency script generated: %s\n", output_file);
        if (format == OUTPUT_SHELL) {
            printf("Run with: bash %s\n", output_file);
        }
    }

    xr_xir_compile_source_product_free(product);
    if (output_failed) {
        xr_cli_error("deps", "cannot write dependency output");
        return XR_CLI_EXIT_FAIL;
    }
    return XR_CLI_EXIT_OK;
}
