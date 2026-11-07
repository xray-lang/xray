/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_c_run.c - Source-backed C emission for independent host compiler tests
 *
 * KEY CONCEPT:
 *   Destroy frontend inputs before C emission and LIR before writing the
 *   owned output. The host test runner compiles and executes that output.
 */
#include "lir/xlir_c.h"
#include "lir/xlir_source.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    bool optimize = false, valid = argc >= 3 && argc <= 6;
    const char *stdlib = NULL;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--optimize") && !optimize) optimize = true;
        else if (!strcmp(argv[i], "--stdlib") && !stdlib && i + 1 < argc) stdlib = argv[++i];
        else valid = false;
    }
    if (!valid) {
        fputs("usage: lir_c_run <absolute-source.xr> <output.c> [--optimize] "
            "[--stdlib <directory>]\n", stderr);
        return 2;
    }
    int result = 1;
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    XrLirLowered lowered = {0};
    XrLirSourceFailure failure = {0};
    XrLirCOutput output = {0};
    if (xr_compile_resources_new(&limits, &resources) != XR_COMPILE_RESOURCE_OK) {
        fputs("cannot prepare source compilation\n", stderr);
        return 1;
    }
    XrLirSourceRequest input = {resources, argv[1], stdlib, optimize, NULL, NULL};
    XrLirSourceStatus status = xr_lir_compile_source(&input, &lowered, &failure);
    if (status == XR_LIR_SOURCE_CHECK_FAILED) {
        fprintf(stderr, "source %d:%d: %s\n", failure.source.line,
            failure.source.column, failure.source.message);
    } else if (status == XR_LIR_SOURCE_SPECIALIZATION_FAILED) {
        fputs("source specialization failed\n", stderr);
    } else if (status == XR_LIR_SOURCE_LOWER_FAILED) {
        fprintf(stderr, "lowering: %s\n", failure.lower.diagnostic.message);
    } else if (status != XR_LIR_SOURCE_OK) {
        fputs("cannot prepare source compilation\n", stderr);
    } else {
        XrLirCRequest request = {lowered.module, lowered.entry, resources, argv[1], NULL};
        XrLirCDiagnostic diagnostic = {0};
        if (xr_lir_emit_c(&request, &output, &diagnostic) != XR_LIR_C_OK) {
            fprintf(stderr, "C emission: %s\n", diagnostic.message);
        } else {
            xr_lir_lowered_free(&lowered);
            FILE *file = fopen(argv[2], "wb");
            if (!file) {
                fputs("cannot open C output\n", stderr);
            } else {
                bool written = fwrite(output.bytes, 1, output.length, file) == output.length;
                bool closed = fclose(file) == 0;
                if (written && closed) result = 0;
                else fputs("cannot write C output\n", stderr);
            }
        }
    }
    xr_lir_c_output_free(&output);
    xr_lir_lowered_free(&lowered);
    xr_lir_source_failure_free(&failure);
    xr_compile_resources_release(resources);
    return result;
}
