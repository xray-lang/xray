/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_values.h"
#include "xray_native_string_value.h"
#define TEST_ROLE_SESSION_CLOSE_CONTROL 1
#define TEST_ROLE_SESSION_HOST_ONLY 1
#include "test_role_session_selection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const XrtNativeTestFile xrl_test_file;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "test session host failed at line %d: %s\n", __LINE__, #condition); \
    exit(2); \
} } while (0)

int main(int argc, char **argv) {
    bool retry_first = argc == 2 && !strcmp(argv[1], "--retry-first");
    bool defer_close = argc == 2 && !strcmp(argv[1], "--defer-close");
    CHECK(argc == 1 || retry_first || defer_close);
    XrtTestSession *session = NULL;
    XrtManagedStatus created = defer_close
        ? xrt_test_session_create_native_with_metadata_close(&xrl_test_file, session_close_attempt, &session)
        : xrt_test_session_create_native(&xrl_test_file, &session);
    CHECK(created == XRT_MANAGED_OK);
    XrtLocationOwner *locations = xrt_test_session_locations(session);
    CHECK(xrt_locations_retain(locations));
    XrtExecutionFailure first = {0};
    CHECK(xrt_test_session_run_native(session, xrl_test_file.action_count, &first) == XRT_EXECUTION_INVALID);
    SessionCloseControl close = {0};
    bool failed = defer_close;
    if (defer_close) {
        CHECK(xrl_test_file.action_count == 2);
        session_close_control = &close;
        const XrtTestRetirementHook hook = {session_close_retire, &close};
        CHECK(xrt_test_session_run_native_with_retirement_hook(session, 0, &hook, &first) == XRT_EXECUTION_FAILURE);
        CHECK(first.kind == XRT_FAILURE_MESSAGE_PANIC && close.attempts == 1 && close.metadata->handle == 17);
        XrtExecutionFailure blocked = {0};
        CHECK(xrt_test_session_run_native(session, 1, &blocked) == XRT_EXECUTION_FAILURE);
        CHECK(blocked.kind == XRT_FAILURE_VALUE && blocked.detail.value.status == XR_XIR_VALUE_IO &&
            blocked.detail.value.source_id == 0 && close.attempts == 2 && close.metadata->handle == 17);
        xrt_execution_failure_drop(&blocked);
        CHECK(xrt_test_session_run_native(session, 1, &blocked) == XRT_EXECUTION_COMPLETED);
        CHECK(!xrt_value_root_active() && close.attempts == 3);
    }
    for (uint32_t i = 0; !defer_close && i < xrl_test_file.action_count; ++i) {
        XrtExecutionFailure failure = {0};
        XrtExecutionStatus status = xrt_test_session_run_native(session, i, &failure);
        if (status == XRT_EXECUTION_FAILURE) {
            if (!failed) first = failure;
            else xrt_execution_failure_drop(&failure);
            failed = true;
        } else CHECK(status == XRT_EXECUTION_COMPLETED || failed);
        CHECK(!xrt_value_root_active());
    }
    if (retry_first) {
        XrtExecutionFailure discarded = {0};
        CHECK(failed &&
            xrt_test_session_run_native(session, 0, &discarded) == XRT_EXECUTION_INVALID);
    }
    CHECK(xrt_test_session_free(&session) == XR_OS_IO_OK && !session);
    if (failed) {
        CHECK(first.kind == XRT_FAILURE_MESSAGE_PANIC);
        const XrtStringBacking *message = (const XrtStringBacking *)first.detail.message.message;
        CHECK(message && xrt_message_panic_report(first.detail.message.code, message->bytes,
            message->length, xrt_locations_get(locations, first.detail.message.source_id)));
        xrt_execution_failure_drop(&first);
    }
    xrt_locations_release(locations);
    if (defer_close) CHECK(session_close_finish(&close));
    return failed ? 1 : 0;
}
