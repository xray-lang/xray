/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_execution.c - Real leaf recovery and nested synchronous binding
 *
 * KEY CONCEPT:
 *   Controlled host callbacks test the common boundary, independently of VM
 *   dispatch. No standalone global is supplied to satisfy this executable.
 */
#include "runtime/native/xnative_execution.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#else
#include <unistd.h>
#endif
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

typedef struct Context {
    XrtLocationOwner *owner;
    XrtOutputFailure failure;
    unsigned visits, after;
    bool fail_inner;
    XrtExecutionStatus inner_status;
    struct Context *inner;
} Context;

static void normal_root(void *data) { ++((Context *)data)->visits; }
static void output_root(void *data) {
    Context *context = data;
    ++context->visits;
    xrt_print_i64(17, 1);
    ++context->after;
}
static void nested_root(void *data) {
    Context *context = data;
    context->inner_status = xrt_run_embedded(context->inner->owner, &context->inner->failure,
        context->fail_inner ? output_root : normal_root, context->inner);
    output_root(context);
}

static XrtLocationOwner *locations(const char *path) {
    XrtSourceLocation location = {path, strlen(path), 7, 9, 8, 11};
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(&location, 1, &owner) == XRT_LOCATION_OK);
    return owner;
}

#if defined(XR_OS_WINDOWS)
typedef HANDLE SavedOutput;
static SavedOutput disable_output(void) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CHECK(SetStdHandle(STD_OUTPUT_HANDLE, INVALID_HANDLE_VALUE));
    return output;
}
static void restore_output(SavedOutput output) { CHECK(SetStdHandle(STD_OUTPUT_HANDLE, output)); }
#else
typedef int SavedOutput;
static SavedOutput disable_output(void) {
    int output = dup(STDOUT_FILENO);
    CHECK(output >= 0 && close(STDOUT_FILENO) == 0);
    return output;
}
static void restore_output(SavedOutput output) {
    CHECK(dup2(output, STDOUT_FILENO) >= 0 && close(output) == 0);
    clearerr(stdout);
}
#endif

int main(void) {
    Context *outer = xr_calloc(1, sizeof(*outer));
    Context *inner = xr_calloc(1, sizeof(*inner));
    CHECK(outer && inner);
    outer->owner = locations("outer-source.xr");
    inner->owner = locations("inner-source.xr");
    outer->inner = inner;
    outer->failure.source_id = 99;
    CHECK(xrt_run_embedded(NULL, &outer->failure, normal_root, outer) == XRT_EXECUTION_INVALID);
    CHECK(!outer->visits && outer->failure.source_id == 99);
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, NULL, outer) == XRT_EXECUTION_INVALID);
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, normal_root, outer) == XRT_EXECUTION_COMPLETED);
    CHECK(outer->visits == 1 && outer->failure.source_id == 99);
    for (unsigned failure = 0; failure < 2; ++failure) {
        outer->after = inner->after = 0;
        outer->fail_inner = failure != 0;
        inner->failure.source_id = 99;
        SavedOutput saved = disable_output();
        XrtExecutionStatus status = xrt_run_embedded(outer->owner, &outer->failure, nested_root, outer);
        restore_output(saved);
        CHECK(status == XRT_EXECUTION_OUTPUT_FAILURE && !outer->after && !inner->after);
        CHECK(outer->failure.io.failed && outer->failure.io.code && outer->failure.source_id == 1);
        CHECK(outer->failure.io.domain != XRT_IO_NONE);
        CHECK(outer->inner_status == (failure ? XRT_EXECUTION_OUTPUT_FAILURE : XRT_EXECUTION_COMPLETED));
        if (failure) CHECK(inner->failure.io.failed && inner->failure.source_id == 1);
        else CHECK(inner->failure.source_id == 99);
        // The prior failed invocation leaves no expired recovery target behind.
        CHECK(xrt_run_embedded(outer->owner, &outer->failure, normal_root, outer) == XRT_EXECUTION_COMPLETED);
    }
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, output_root, outer) == XRT_EXECUTION_COMPLETED);
    CHECK(outer->after == 1);
    xrt_locations_release(inner->owner);
    xrt_locations_release(outer->owner);
    xr_free(inner);
    xr_free(outer);
    fputs("Synchronous runtime scope passed\n", stderr);
    return 0;
}
