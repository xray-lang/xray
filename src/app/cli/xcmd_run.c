/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_run.c - Owned LIR and register VM execution for run
 *
 * KEY CONCEPT:
 *   Project authority and frontend owners retire before the register program
 *   executes. The result owns every failure it can report after execution.
 */
#include "xcli.h"
#include "xcli_lir.h"
#include "vm/register/xrbc_lir.h"
#include "shared/xr_value_format_core.h"
#include <stdio.h>
#include <string.h>
#if XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

static int run_panic_write(void *context, const void *bytes, size_t length) {
    (void)context;
    return !xrt_io_write(2, bytes, length).failed;
}

static int run_report(const XrRbcOutcome *outcome, XrRbcStatus status) {
    if (status == XR_RBC_OK) return XR_CLI_EXIT_OK;
    XrtArithmeticPanic panic;
    const XrtLocationOwner *locations;
    if (xr_rbc_outcome_arithmetic_panic(outcome, &panic, &locations)) {
        XrValueFormatSink sink = {NULL, run_panic_write};
        if (!xr_value_format_panic(sink, panic.code, 0, 0, 0, 0, NULL, 0, 0))
            fputs("cannot render uncaught arithmetic panic\n", stderr);
    } else {
        XrtHostClockFailure clock;
        if (xr_rbc_outcome_host_clock_failure(outcome, &clock, &locations)) {
            const XrtSourceLocation *source = xrt_locations_get(locations, clock.source_id);
            fprintf(stderr, "host clock %u failed at %s:%u:%u\n", clock.kind,
                source ? source->path : "unknown source", source ? source->line : 0,
                source ? source->column : 0);
        } else fprintf(stderr, "register VM execution failed (%u)\n", (unsigned)status);
    }
    return XR_CLI_EXIT_FAIL;
}

XR_FUNC int cmd_run(const XrCliInvocation *inv) {
    if (!inv) return XR_CLI_EXIT_INTERNAL;
#if XR_OS_WINDOWS
    if (_setmode(_fileno(stdout), _O_BINARY) == -1 ||
        _setmode(_fileno(stderr), _O_BINARY) == -1) return XR_CLI_EXIT_INTERNAL;
#endif
    if (inv->positional_count != 1 || !inv->positionals || inv->passthrough_argc) {
        xr_cli_error("run", "one source file and no passthrough arguments are required");
        return XR_CLI_EXIT_USAGE;
    }
    const char *input = inv->positionals[0];
    size_t length = input ? strlen(input) : 0;
    if (length <= 3 || strcmp(input + length - 3, ".xr")) {
        xr_cli_error("run", "an exact .xr source path is required");
        return XR_CLI_EXIT_FAIL;
    }
    if (xr_cli_opt_bool(&inv->options, "sandbox")) {
        xr_cli_error("run", "sandbox execution is temporarily unavailable");
        return XR_CLI_EXIT_FAIL;
    }
    XrCompileResources *resources = NULL;
    XrLirLowered lowered = {0};
    int result = xr_cli_lir_compile(input, &resources, &lowered);
    XrRbcProgram *program = NULL;
    if (result == XR_CLI_EXIT_OK) {
        XrRbcCompileDiagnostic diagnostic = {0};
        if (xr_rbc_compile(lowered.module, lowered.entry, &program, &diagnostic) != XR_RBC_COMPILE_OK) {
            fprintf(stderr, "%s:%u:%u: %s\n", diagnostic.path[0] ? diagnostic.path : input,
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
        result = run_report(outcome, status);
        xr_rbc_outcome_free(&outcome);
    }
    xr_rbc_program_destroy(program);
    return result;
}
