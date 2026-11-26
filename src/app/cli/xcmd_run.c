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
#include <stdlib.h>
#include <string.h>
#if XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

static int run_panic_write(void *context, const void *bytes, size_t length) {
    (void)context;
    return !xrt_io_write(2, bytes, length).failed;
}

static void run_try_trace(const XrRbcOutcome *outcome) {
    size_t count=xr_rbc_outcome_try_trace_count(outcome);
    if (!count) return;
    fputs("Stack trace:\n",stderr);
    for (size_t i=0;i<count;++i) {
        const XrtSourceLocation *site=xr_rbc_outcome_try_trace_at(outcome,i);
        if (!site) continue;
        fputs("    at ",stderr);
        (void)fwrite(site->path,1,site->path_length,stderr);
        fprintf(stderr,":%u:%u\n",site->line,site->column);
    }
}

static int run_report(const XrRbcOutcome *outcome, XrRbcStatus status) {
    if (status == XR_RBC_OK) return XR_CLI_EXIT_OK;
    XrtArithmeticPanic panic;
    XrtTempFailure temp = {0};
    const XrtLocationOwner *locations;
    uint32_t message_code=0,message_source=0;const char *message=NULL;size_t message_length=0;
    const XrtErrorSnapshot *error=xr_rbc_outcome_uncaught_error(outcome);
    const XrtTryAssertFailure *assertion=xr_rbc_outcome_try_assert(outcome);
    if (error) {
        if (!xrt_uncaught_error_report(error)) fputs("XR_RUN_6005: cannot render uncaught error\n",stderr);
    } else if (assertion) {
        if (!xrt_try_assert_report(assertion)) fputs("cannot render uncaught try assertion\n",stderr);
        else run_try_trace(outcome);
    } else if(xr_rbc_outcome_message_panic(outcome,&message_code,&message,&message_length,&message_source,&locations)) {
        if(!xrt_message_panic_report(message_code,message,message_length,xrt_locations_get(locations,message_source)))
            fputs("cannot render uncaught message panic\n",stderr);
    } else if (xr_rbc_outcome_arithmetic_panic(outcome, &panic, &locations)) {
        XrValueFormatSink sink = {NULL, run_panic_write};
        if (!xr_value_format_panic(sink, panic.code, 0, 0, 0, 0, NULL, 0, 0))
            fputs("cannot render uncaught arithmetic panic\n", stderr);
    } else {
        XrtHostClockFailure clock;
        XrXirValueStatus value_status;
        uint32_t source_id;
        if (xr_rbc_outcome_null_unwrap(outcome, &source_id, &locations)) {
            xrt_null_unwrap_report(xrt_locations_get(locations, source_id));
        } else if (xr_rbc_outcome_host_clock_failure(outcome, &clock, &locations)) {
            const XrtSourceLocation *source = xrt_locations_get(locations, clock.source_id);
            fprintf(stderr, "host clock %u failed at %s:%u:%u\n", clock.kind,
                source ? source->path : "unknown source", source ? source->line : 0,
                source ? source->column : 0);
        } else if (xr_rbc_outcome_value_failure(outcome, &value_status, &source_id, &locations)) {
            uint32_t close_id = xr_rbc_outcome_directory_close_source(outcome);
            xrt_value_failure_report_with_directory_close(value_status,
                xrt_locations_get(locations, source_id), xrt_locations_get(locations, close_id));
        } else if (xr_rbc_outcome_take_temp(outcome, &temp)) {
            xrt_temp_failure_report(xr_rbc_outcome_temp_location(outcome));
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
    /* Preparation moves the admitted value code into its execution image. */
    const bool receives_temp = xr_rbc_program_receives_text_result(program);
    if (result == XR_CLI_EXIT_OK) {
        XrRbcExecution *execution = NULL;
        XrRbcStatus status = xr_rbc_program_prepare(program, NULL, 0, &execution);
        const char *backtrace=getenv("XRAY_BACKTRACE");
        if (status==XR_RBC_OK && backtrace && !strcmp(backtrace,"1")) xr_rbc_execution_enable_try_trace(execution);
        XrRbcOutcome *outcome = status == XR_RBC_OK ? (receives_temp ?
            xr_rbc_execution_run_receiving_temp(execution) : xr_rbc_execution_run(execution)) : NULL;
        if (status == XR_RBC_OK) status = xr_rbc_outcome_status(outcome);
        XrOsIoStatus retired = xr_rbc_execution_free(&execution);
        xr_rbc_program_destroy(program);
        program = NULL;
        result = run_report(outcome, status);
        xr_rbc_outcome_free(&outcome);
        if (retired != XR_OS_IO_OK) {
            if (result == XR_CLI_EXIT_OK) {
                fprintf(stderr, "register VM retirement failed (%u)\n", (unsigned)retired);
                result = XR_CLI_EXIT_FAIL;
            }
            /* The execution still owns the exact handle at process exit. */
            _Exit(result);
        }
    }
    xr_rbc_program_destroy(program);
    return result;
}
