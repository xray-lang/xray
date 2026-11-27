/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_test_suite_native.c - Execute the suite from its emitted C
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "lir_test_suite_catalog.h"
#include "base/xmalloc.h"
#include "runtime/native/xnative_execution.h"
#include "xray_native_string_value.h"
#include <stdlib.h>

extern const XrtNativeTestFile xrl_test_file;
extern const SuiteCatalog xrl_suite_catalog;

static XrtSynchronousStateRoot selected;
static bool entered;

static void observe_action(void *context, XrtModuleState *state) {
    entered = true;
    selected(context, state);
}

static bool run_tests(XrtTestSession *session, SuiteResults *results) {
    for (uint32_t i = 0; i < xrl_suite_catalog.count; ++i) {
        const SuiteRow *row = &xrl_suite_catalog.rows[i];
        if (row->role == SUITE_SKIP) {
            suite_record(results, "native", row, false);
            continue;
        }
        XrtExecutionFailure failure = {0};
        selected = xrl_test_file.actions[row->action];
        entered = false;
        XrtExecutionStatus status = xrt_test_session_run_native(session, row->action, &failure);
        if (!entered || (status != XRT_EXECUTION_COMPLETED && status != XRT_EXECUTION_FAILURE)) {
            fprintf(stderr, "native test action could not start (status %u)\n", (unsigned)status);
            xrt_execution_failure_drop(&failure);
            return false;
        }
        if (failure.kind == XRT_FAILURE_MESSAGE_PANIC) {
            const XrtStringBacking *message = (const XrtStringBacking *)failure.detail.message.message;
            const XrtSourceLocation *source = xrt_locations_get(xrt_test_session_locations(session),
                failure.detail.message.source_id);
            if (message)
                (void)xrt_message_panic_report(failure.detail.message.code, message->bytes, message->length, source);
        }
        suite_record(results, "native", row, status == XRT_EXECUTION_COMPLETED);
        xrt_execution_failure_drop(&failure);
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fputs("usage: lir_test_suite_native REPORT\n", stderr);
        return 2;
    }
    if (!xrl_test_file.initializer || !xrl_test_file.actions ||
        !suite_catalog_valid(&xrl_suite_catalog, xrl_test_file.action_count)) {
        fputs("generated suite catalog does not match its action table\n", stderr);
        return 1;
    }
    XrtNativeTestFile file = xrl_test_file;
    XrtSynchronousStateRoot *observed = xr_calloc(file.action_count, sizeof(*observed));
    if (!observed) return 1;
    for (uint32_t i = 0; i < file.action_count; ++i) {
        if (!file.actions[i]) {
            fputs("generated suite action table has a missing entry\n", stderr);
            xr_free(observed);
            return 1;
        }
        observed[i] = observe_action;
    }
    file.actions = observed;
    XrtTestSession *session = NULL;
    if (xrt_test_session_create_native(&file, &session) != XRT_MANAGED_OK) {
        fputs("cannot create native suite execution\n", stderr);
        xr_free(observed);
        return 1;
    }
    SuiteResults results = {0};
    bool executed = run_tests(session, &results);
    if (xrt_test_session_free(&session) != XR_OS_IO_OK) {
        fputs("cannot retire native suite execution\n", stderr);
        _Exit(1);
    }
    xr_free(observed);
    if (!executed) return 1;
    if (!suite_write_report(argv[1], &xrl_suite_catalog, results)) {
        fputs("cannot publish native suite report\n", stderr);
        return 1;
    }
    suite_summary(results);
    return results.failed ? 1 : 0;
}
