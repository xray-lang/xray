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
    CHECK(argc == 1 || retry_first);
    XrtTestSession *session = NULL;
    CHECK(xrt_test_session_create_native(&xrl_test_file, &session) == XRT_MANAGED_OK);
    XrtLocationOwner *locations = xrt_test_session_locations(session);
    CHECK(xrt_locations_retain(locations));
    XrtExecutionFailure first = {0};
    CHECK(xrt_test_session_run_native(session, xrl_test_file.action_count, &first) == XRT_EXECUTION_INVALID);
    bool failed = false;
    for (uint32_t i = 0; i < xrl_test_file.action_count; ++i) {
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
    return failed ? 1 : 0;
}
