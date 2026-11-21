/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_test.c - Run every listed case on the register VM and report per case
 *
 * KEY CONCEPT:
 *   A test case passes only when its program compiles, runs and completes
 *   without a panic. The command owns nothing beyond one case at a time, so a
 *   failure is reported against the case that produced it.
 *
 * SCOPE: explicit `.xr` paths only. Directory discovery is refused with a usage
 * error rather than silently doing nothing, and `--jobs` is accepted but the
 * cases run serially because each one owns a whole compile session.
 */
#include "xcli.h"
#include "xcli_lir.h"
#include "vm/register/xrbc_lir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

typedef struct TestTally {
    size_t passed, failed, skipped;
} TestTally;

static bool test_path_has_suffix(const char *path, const char *suffix) {
    if (!path || !suffix) return false;
    size_t length = strlen(path), tail = strlen(suffix);
    return length > tail && !strcmp(path + length - tail, suffix);
}

static bool test_selected(const char *path, const char *filter) {
    return !filter || !*filter || strstr(path, filter) != NULL;
}

/* One case owns its compile session and its VM; nothing survives the call. */
static bool test_run_case(const char *path, int *exit_code) {
    XrCompileResources *resources = NULL;
    XrLirLowered lowered = {0};
    int result = xr_cli_lir_compile(path, &resources, &lowered);
    XrRbcProgram *program = NULL;
    if (result == XR_CLI_EXIT_OK) {
        XrRbcCompileDiagnostic diagnostic = {0};
        if (xr_rbc_compile(lowered.module, lowered.entry, &program, &diagnostic) != XR_RBC_COMPILE_OK) {
            fprintf(stderr, "%s:%u:%u: %s\n", diagnostic.path[0] ? diagnostic.path : path,
                diagnostic.line, diagnostic.column, diagnostic.message);
            result = XR_CLI_EXIT_FAIL;
        }
    }
    xr_lir_lowered_free(&lowered);
    xr_compile_resources_release(resources);
    if (result == XR_CLI_EXIT_OK) {
        XrRbcExecution *execution = NULL;
        XrRbcStatus status = xr_rbc_program_prepare(program, NULL, 0, &execution);
        XrRbcOutcome *outcome = status == XR_RBC_OK ? xr_rbc_execution_run(execution) : NULL;
        if (status == XR_RBC_OK) status = xr_rbc_outcome_status(outcome);
        xr_rbc_execution_free(&execution);
        xr_rbc_program_destroy(program);
        program = NULL;
        if (status != XR_RBC_OK) {
            fprintf(stderr, "%s: case did not complete (%u)\n", path, (unsigned) status);
            result = XR_CLI_EXIT_FAIL;
        }
        xr_rbc_outcome_free(&outcome);
    }
    xr_rbc_program_destroy(program);
    *exit_code = result;
    return result == XR_CLI_EXIT_OK;
}

static bool test_write_report(const char *path, const XrCliInvocation *inv, const TestTally *tally,
    const char **failures, size_t failure_count) {
    (void) inv;
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    fprintf(file, "{\"schema\":1,\"passed\":%zu,\"failed\":%zu,\"skipped\":%zu,\"failures\":[",
        tally->passed, tally->failed, tally->skipped);
    for (size_t i = 0; i < failure_count; ++i)
        fprintf(file, "%s\"%s\"", i ? "," : "", failures[i]);
    fprintf(file, "]}\n");
    return !fclose(file);
}

XR_FUNC int cmd_test(const XrCliInvocation *inv) {
    if (!inv) return XR_CLI_EXIT_INTERNAL;
#if XR_OS_WINDOWS
    if (_setmode(_fileno(stdout), _O_BINARY) == -1 ||
        _setmode(_fileno(stderr), _O_BINARY) == -1) return XR_CLI_EXIT_INTERNAL;
#endif
    if (xr_cli_opt_bool(&inv->options, "sandbox")) {
        xr_cli_error("test", "sandbox execution is temporarily unavailable");
        return XR_CLI_EXIT_FAIL;
    }
    if (!inv->positional_count || !inv->positionals) {
        xr_cli_error("test", "at least one .xr case path is required; directory discovery is not implemented");
        return XR_CLI_EXIT_USAGE;
    }
    const char *filter = xr_cli_opt_string(&inv->options, "filter", NULL);
    const char *report = xr_cli_opt_string(&inv->options, "report", NULL);
    bool verbose = xr_cli_opt_bool(&inv->options, "verbose");
    bool quiet = xr_cli_opt_bool(&inv->options, "quiet");
    bool fail_fast = xr_cli_opt_bool(&inv->options, "fail-fast");
    TestTally tally = {0, 0, 0};
    const char **failures = calloc((size_t) inv->positional_count, sizeof(*failures));
    if (!failures) return XR_CLI_EXIT_INTERNAL;
    size_t failure_count = 0;
    for (int i = 0; i < inv->positional_count; ++i) {
        const char *path = inv->positionals[i];
        if (!test_path_has_suffix(path, ".xr")) {
            xr_cli_error("test", "every case path must be a .xr file");
            return XR_CLI_EXIT_USAGE;
        }
        if (!test_selected(path, filter)) {
            ++tally.skipped;
            if (verbose && !quiet) printf("skip %s\n", path);
            continue;
        }
        int exit_code = XR_CLI_EXIT_FAIL;
        bool passed = test_run_case(path, &exit_code);
        if (passed) {
            ++tally.passed;
            if (verbose && !quiet) printf("pass %s\n", path);
        } else {
            ++tally.failed;
            failures[failure_count++] = path;
            if (!quiet) printf("fail %s\n", path);
            if (fail_fast) break;
        }
    }
    if (!quiet)
        printf("xray test: %zu passed, %zu failed, %zu skipped\n", tally.passed, tally.failed, tally.skipped);
    if (report && !test_write_report(report, inv, &tally, failures, failure_count))
        xr_cli_error("test", "cannot write the report file");
    free(failures);
    return tally.failed ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}
