/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_vm_run.c - Source-backed register-bytecode test driver
 *
 * KEY CONCEPT:
 *   Execute only the entry returned by lowering, after all borrowed compiler
 *   inputs have been retired. Unsupported closures never select an executor.
 */
#include "lir/xlir_source.h"
#include "lir/xlir_test_plan.h"
#define TEST_ROLE_SESSION_CLOSE_CONTROL 1
#include "../lir/test_role_session_selection.h"
#include "vm/register/xrbc_lir.h"
#include "lir_vm_result_report.h"
#include <stdio.h>
#include <string.h>

static void compile_file(const char *path, bool optimize, const char *stdlib,
    const char *test_name, bool test_plan, const char *session_name, bool defer_close,
    XrRbcProgram **program, XrRbcTestSession **session, uint32_t *action_count) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    XrLirLowered lowered = {0};
    XrLirTestPlan *plan = NULL;
    XrLirSourceFailure failure = {0};
    XrLirFunction *actions = NULL;
    if (xr_compile_resources_new(&limits, &resources) != XR_COMPILE_RESOURCE_OK) {
        fprintf(stderr, "lir_vm_run: cannot prepare source input: %s\n", path);
        return;
    }
    XrLirSourceRequest request = {resources, path, stdlib, optimize, NULL, NULL, XR_XIR_HOST_TARGET};
    XrLirSourceStatus status;
    if (test_plan || session_name)
        status = xr_lir_compile_test_plan(&request, &plan, &failure);
    else if (test_name)
        status = xr_lir_compile_test_source(&request, test_name, &lowered, &failure);
    else
        status = xr_lir_compile_source(&request, &lowered, &failure);
    if (status == XR_LIR_SOURCE_OK && session_name &&
        !select_session_actions(plan, session_name, &actions, action_count))
        status = XR_LIR_SOURCE_TEST_SELECTION_FAILED;
    if (status == XR_LIR_SOURCE_CHECK_FAILED) {
        fprintf(stderr, "lir_vm_run: source %s:%d:%d: %s\n",
            failure.source_failure.path ? failure.source_failure.path : path,
            failure.source.line, failure.source.column, failure.source.message);
    } else if (status == XR_LIR_SOURCE_TEST_SELECTION_FAILED) {
        fputs("no matching root-module @test declaration\n", stderr);
    } else if (status == XR_LIR_SOURCE_SPECIALIZATION_FAILED) {
        fprintf(stderr, "lir_vm_run: specialization failed (%u)\n",
            (unsigned)failure.specialization_status);
    } else if (status == XR_LIR_SOURCE_LOWER_FAILED) {
        fprintf(stderr, "lir_vm_run: lower %s:%u:%u: %s [function=%u block=%u instruction=%u]\n",
            failure.lower.owned_path ? failure.lower.owned_path : path,
            failure.lower.diagnostic.source.line, failure.lower.diagnostic.source.column,
            failure.lower.diagnostic.message, failure.lower.diagnostic.function,
            failure.lower.diagnostic.block, failure.lower.diagnostic.instruction);
    } else if (status != XR_LIR_SOURCE_OK) {
        fprintf(stderr, "lir_vm_run: cannot prepare source input: %s\n", path);
    } else {
        XrRbcCompileDiagnostic diagnostic = {0};
        XrLirFunction entry = plan ? xr_lir_test_plan_initializer(plan) : lowered.entry;
        XrRbcCompileStatus compiled;
        if (defer_close)
            compiled = xr_rbc_test_session_compile_with_metadata_close(entry.module, entry, actions,
                *action_count, session_close_attempt, session, &diagnostic);
        else if (session_name)
            compiled = xr_rbc_test_session_compile(entry.module, entry, actions, *action_count, session, &diagnostic);
        else
            compiled = xr_rbc_compile(entry.module, entry, program, &diagnostic);
        if (compiled != XR_RBC_COMPILE_OK) {
            fprintf(stderr, "lir_vm_run: bytecode %s:%u:%u: %s\n",
                diagnostic.path[0] ? diagnostic.path : path,
                diagnostic.line, diagnostic.column, diagnostic.message);
        }
    }
    xr_lir_source_failure_free(&failure);
    xr_lir_lowered_free(&lowered);
    xr_lir_test_plan_free(plan);
    free(actions);
    xr_compile_resources_release(resources);
}

static bool report_test_panic(const XrRbcOutcome *outcome) {
    uint32_t code, source;
    const char *message;
    size_t length;
    const XrtLocationOwner *locations;
    if (!xr_rbc_outcome_message_panic(outcome, &code, &message, &length, &source, &locations))
        return false;
    if (!xrt_message_panic_report(code, message, length, xrt_locations_get(locations, source)))
        fputs("cannot render uncaught message panic\n", stderr);
    return true;
}

static bool run_deferred_close(XrRbcTestSession *session, SessionCloseControl *close, XrRbcOutcome **first) {
    const XrtTestRetirementHook hook = {session_close_retire, close};
    if (xr_rbc_test_session_run_with_retirement_hook(session, 0, &hook, first) != XR_RBC_MESSAGE_PANIC ||
        close->attempts != 1 || !close->metadata || close->metadata->handle != 17) return false;
    XrRbcOutcome *outcome = NULL;
    XrXirValueStatus error;
    uint32_t source;
    const XrtLocationOwner *locations;
    bool valid = xr_rbc_test_session_run(session, 1, &outcome) == XR_RBC_VALUE_FAILURE &&
        xr_rbc_outcome_value_failure(outcome, &error, &source, &locations) &&
        error == XR_XIR_VALUE_IO && source == 0 && locations &&
        close->attempts == 2 && close->metadata->handle == 17;
    xr_rbc_outcome_free(&outcome);
    valid = valid && xr_rbc_test_session_run(session, 1, &outcome) == XR_RBC_OK &&
        xr_rbc_outcome_status(outcome) == XR_RBC_OK && close->attempts == 3 && !xrt_value_root_active();
    xr_rbc_outcome_free(&outcome);
    return valid;
}

static int run_controlled_action(XrRbcTestSession *session, uint32_t action_count, SessionRequestMode mode) {
    const XrtHostExecutionControl invalid = {0};
    XrRbcOutcome *outcome = NULL;
    XrRbcTestActionStage stage = XR_RBC_TEST_ACTION_INVOKE;
    bool valid = action_count == 2 &&
        xr_rbc_test_session_run_controlled(session, 0, &invalid, &outcome, &stage) == XR_RBC_INVALID &&
        !outcome && stage == XR_RBC_TEST_ACTION_REJECTED;
    SessionRequestProbe probe = {.allow=mode == SESSION_REQUEST_ALLOW};
    const XrtHostExecutionControl control = {session_request_current, &probe};
    probe.control = &control;
    if (valid) {
        XrRbcStatus status = xr_rbc_test_session_run_controlled(session, 0, &control, &outcome, &stage);
        valid = stage == XR_RBC_TEST_ACTION_INVOKE && probe.polls &&
            !xrt_execution_host_control() && !xrt_value_root_active();
        if (probe.allow) valid = valid && status == XR_RBC_OK;
        else if (valid) {
            XrXirValueStatus error = XR_XIR_VALUE_OK;
            uint32_t source = 0;
            const XrtLocationOwner *locations = NULL;
            valid = status == XR_RBC_VALUE_FAILURE &&
                xr_rbc_outcome_value_failure(outcome, &error, &source, &locations) &&
                error == XR_XIR_VALUE_CANCELLED && locations;
            if (valid) xrt_value_failure_report(error, xrt_locations_get(locations, source));
        }
    }
    xr_rbc_outcome_free(&outcome);
    unsigned polls = probe.polls;
    valid = valid && xr_rbc_test_session_run(session, 1, &outcome) == XR_RBC_OK &&
        probe.polls == polls && !xrt_execution_host_control() && !xrt_value_root_active();
    xr_rbc_outcome_free(&outcome);
    if (xr_rbc_test_session_free(&session) != XR_OS_IO_OK) valid = false;
    return valid ? (probe.allow ? 0 : 1) : 2;
}

static int run_test_session(XrRbcTestSession *session, uint32_t action_count, bool retry_first, bool defer_close) {
    XrRbcOutcome *first = NULL;
    XrRbcStatus first_status = xr_rbc_test_session_run(session, action_count, &first);
    bool valid = first_status == XR_RBC_INVALID && !first;
    SessionCloseControl close = {0};
    bool failed = defer_close;
    if (defer_close) {
        session_close_control = &close;
        valid = valid && action_count == 2 && run_deferred_close(session, &close, &first);
        first_status = XR_RBC_MESSAGE_PANIC;
    }
    for (uint32_t i = 0; valid && !defer_close && i < action_count; ++i) {
        XrRbcOutcome *outcome = NULL;
        XrRbcStatus status = xr_rbc_test_session_run(session, i, &outcome);
        if (status == XR_RBC_INVALID) valid = failed && !outcome;
        else if (status != XR_RBC_OK && !failed) {
            first = outcome;
            outcome = NULL;
            first_status = status;
            failed = true;
        }
        xr_rbc_outcome_free(&outcome);
    }
    if (valid && retry_first) {
        XrRbcOutcome *discarded = NULL;
        valid = failed && xr_rbc_test_session_run(session, 0, &discarded) == XR_RBC_INVALID && !discarded;
        xr_rbc_outcome_free(&discarded);
    }
    if (xr_rbc_test_session_free(&session) != XR_OS_IO_OK) valid = false;
    int result = valid ? 0 : 2;
    if (valid && failed)
        result = report_test_panic(first) ? 1 : lir_vm_result_report(first, first_status);
    xr_rbc_outcome_free(&first);
    if (defer_close && !session_close_finish(&close)) result = 2;
    return result;
}

int main(int argc, char **argv) {
    bool twice = false, optimize = false, test_plan = false, retry_first = false, defer_close = false;
    SessionRequestMode mode = SESSION_REQUEST_NONE;
    bool valid = argc >= 2 && argc <= 8;
    const char *stdlib = NULL, *test_name = NULL, *session_name = NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--twice") && !twice) twice = true;
        else if (!strcmp(argv[i], "--optimize") && !optimize) optimize = true;
        else if (!strcmp(argv[i], "--stdlib") && !stdlib && i + 1 < argc) stdlib = argv[++i];
        else if (!strcmp(argv[i], "--test") && !test_name && i + 1 < argc) test_name = argv[++i];
        else if (!strcmp(argv[i], "--test-plan") && !test_plan) test_plan = true;
        else if (!strcmp(argv[i], "--test-session") && !session_name && i + 1 < argc)
            session_name = argv[++i];
        else if (!strcmp(argv[i], "--retry-first") && !retry_first) retry_first = true;
        else if (!strcmp(argv[i], "--defer-close") && !defer_close) defer_close = true;
        else if (mode == SESSION_REQUEST_NONE) {
            mode = session_request_mode(argv[i]);
            if (mode == SESSION_REQUEST_NONE) valid = false;
        } else valid = false;
    }
    if (!valid || (test_plan && test_name) || (session_name && (test_plan || test_name || twice)) ||
        ((retry_first || defer_close || mode != SESSION_REQUEST_NONE) && !session_name) ||
        (retry_first && defer_close) || (mode != SESSION_REQUEST_NONE && (retry_first || defer_close))) {
        fputs("usage: lir_vm_run <source.xr> [--twice] [--optimize] [--stdlib <directory>] "
            "[--test NAME | --test-plan | --test-session NAME [--retry-first | --defer-close | --allow-action | --cancel-action]]\n", stderr);
        return 2;
    }
    XrRbcProgram *program = NULL;
    XrRbcTestSession *session = NULL;
    uint32_t action_count = 0;
    compile_file(argv[1], optimize, stdlib, test_name, test_plan, session_name, defer_close,
        &program, &session, &action_count);
    if (session && mode != SESSION_REQUEST_NONE) return run_controlled_action(session, action_count, mode);
    if (session) return run_test_session(session, action_count, retry_first, defer_close);
    if (!program) return 1;
    int result = 0;
    for (unsigned i = 0; i < (twice ? 2u : 1u) && !result; ++i) {
        XrRbcExecution *execution = NULL;
        XrRbcStatus status = xr_rbc_program_prepare(program, NULL, 0, &execution);
        XrRbcOutcome *outcome = status == XR_RBC_OK ? xr_rbc_execution_run(execution) : NULL;
        if (status == XR_RBC_OK) status = xr_rbc_outcome_status(outcome);
        xr_rbc_execution_free(&execution);
        if (i + 1 == (twice ? 2u : 1u)) {
            xr_rbc_program_destroy(program);
            program = NULL;
        }
        result = (test_name || test_plan) && report_test_panic(outcome) ? 1 : lir_vm_result_report(outcome, status);
        xr_rbc_outcome_free(&outcome);
    }
    xr_rbc_program_destroy(program);
    return result;
}
