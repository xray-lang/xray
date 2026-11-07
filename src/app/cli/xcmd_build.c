/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_build.c - LIR emission and ordinary native output publication
 */
#include "xcli.h"
#include "xcli_lir.h"
#include "lir/xlir_c.h"
#include <stdio.h>
#include <string.h>
#if XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

static bool build_options_supported(const XrCliInvocation *inv) {
    bool c_only = xr_cli_opt_bool(&inv->options, "c-only");
    for (int i = 0; i < inv->options.count; ++i) {
        if (!inv->options.present[i]) continue;
        const char *name = inv->options.spec[i].long_name;
        if (!strcmp(name, "output") || !strcmp(name, "c-only") ||
            !strcmp(name, "native") || !strcmp(name, "verbose")) continue;
        if (!c_only && !strcmp(name, "cc")) continue;
        fprintf(stderr, "unsupported build option --%s\n", name);
        return false;
    }
    return true;
}

XR_FUNC int cmd_build(const XrCliInvocation *inv) {
    if (!inv) return XR_CLI_EXIT_INTERNAL;
#if XR_OS_WINDOWS
    if (_setmode(_fileno(stdout), _O_BINARY) == -1 ||
        _setmode(_fileno(stderr), _O_BINARY) == -1) return XR_CLI_EXIT_INTERNAL;
#endif
    if (inv->positional_count != 1 || !inv->positionals || inv->passthrough_argc) {
        xr_cli_error("build", "one source file and no passthrough arguments are required");
        return XR_CLI_EXIT_USAGE;
    }
    const char *input = inv->positionals[0];
    size_t length = input ? strlen(input) : 0;
    if (length <= 3 || strcmp(input + length - 3, ".xr")) {
        xr_cli_error("build", "an exact .xr source path is required");
        return XR_CLI_EXIT_FAIL;
    }
    if (!build_options_supported(inv)) return XR_CLI_EXIT_FAIL;
    bool c_only = xr_cli_opt_bool(&inv->options, "c-only");
#if XR_OS_WINDOWS
    const char *default_output = c_only ? "app.c" : "app.exe";
#else
    const char *default_output = c_only ? "app.c" : "app";
#endif
    const char *destination = xr_cli_opt_string(&inv->options, "output", default_output);
    XrCompileResources *resources = NULL;
    XrLirLowered lowered = {0};
    XrLirCOutput output = {0};
    int result = xr_cli_lir_compile(input, &resources, &lowered);
    if (result == XR_CLI_EXIT_OK) {
        XrLirCRequest request = {lowered.module, lowered.entry, resources, input, NULL};
        XrLirCDiagnostic diagnostic = {0};
        if (xr_lir_emit_c_standalone(&request, &output, &diagnostic) != XR_LIR_C_OK) {
            fprintf(stderr, "%s:%u:%u: %s\n", diagnostic.source.path ? diagnostic.source.path : input,
                diagnostic.source.line, diagnostic.source.column, diagnostic.message);
            result = XR_CLI_EXIT_FAIL;
        }
    }
    xr_lir_lowered_free(&lowered);
    if (result == XR_CLI_EXIT_OK) {
        if (c_only) {
            result = xr_cli_lir_publish(resources, destination, output.bytes, output.length, false);
        } else {
            const char *compiler = xr_cli_opt_string(&inv->options, "cc", NULL);
            bool verbose = xr_cli_opt_bool(&inv->options, "verbose") || (inv->ctx && inv->ctx->verbose);
            XrCliNativeOptions options = {compiler, NULL, verbose};
            result = xr_cli_lir_native(resources, &options, output.bytes, output.length, destination);
        }
    }
    xr_lir_c_output_free(&output);
    xr_compile_resources_release(resources);
    if (result == XR_CLI_EXIT_OK) {
        if (printf("%s: %s\n", c_only ? "Generated" : "Built", destination) < 0 || fflush(stdout))
            result = XR_CLI_EXIT_FAIL;
    }
    return result;
}
