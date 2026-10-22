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
    XrtExecutionFailure failure;
    unsigned visits, after;
    uint32_t panic_code, source_id;
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
static void panic_root(void *data) {
    Context *context = data;
    ++context->visits;
    xrt_arithmetic_panic(context->panic_code, context->source_id);
}
static void nested_root(void *data) {
    Context *context = data;
    context->inner_status = xrt_run_embedded(context->inner->owner, &context->inner->failure,
        context->inner->panic_code ? panic_root : context->fail_inner ? output_root : normal_root,
        context->inner);
    if (context->panic_code) panic_root(context);
    else output_root(context);
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
    memset(&outer->failure, 0xa5, sizeof(outer->failure));
    unsigned char prior[sizeof(outer->failure)];
    memcpy(prior, &outer->failure, sizeof(prior));
    CHECK(xrt_run_embedded(NULL, &outer->failure, normal_root, outer) == XRT_EXECUTION_INVALID);
    CHECK(!outer->visits && !memcmp(prior, &outer->failure, sizeof(prior)));
    CHECK(xrt_run_embedded(outer->owner, NULL, normal_root, outer) == XRT_EXECUTION_INVALID);
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, NULL, outer) == XRT_EXECUTION_INVALID);
    CHECK(!outer->visits && !memcmp(prior, &outer->failure, sizeof(prior)));
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, normal_root, outer) == XRT_EXECUTION_COMPLETED);
    CHECK(outer->visits == 1 && !memcmp(prior, &outer->failure, sizeof(prior)));
    for (unsigned failure = 0; failure < 2; ++failure) {
        outer->after = inner->after = 0;
        outer->fail_inner = failure != 0;
        memset(&inner->failure, 0xa5, sizeof(inner->failure));
        SavedOutput saved = disable_output();
        XrtExecutionStatus status = xrt_run_embedded(outer->owner, &outer->failure, nested_root, outer);
        restore_output(saved);
        CHECK(status == XRT_EXECUTION_FAILURE && !outer->after && !inner->after);
        CHECK(outer->failure.kind == XRT_FAILURE_OUTPUT);
        XrtOutputFailure *output = &outer->failure.detail.output;
        CHECK(output->io.failed && output->io.code && output->source_id == 1);
        CHECK(output->io.domain != XRT_IO_NONE);
        CHECK(outer->inner_status == (failure ? XRT_EXECUTION_FAILURE : XRT_EXECUTION_COMPLETED));
        if (failure) {
            CHECK(inner->failure.kind == XRT_FAILURE_OUTPUT);
            CHECK(inner->failure.detail.output.io.failed && inner->failure.detail.output.source_id == 1);
        } else CHECK(!memcmp(prior, &inner->failure, sizeof(prior)));
        // The prior failed invocation leaves no expired recovery target behind.
        CHECK(xrt_run_embedded(outer->owner, &outer->failure, normal_root, outer) == XRT_EXECUTION_COMPLETED);
    }
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, output_root, outer) == XRT_EXECUTION_COMPLETED);
    CHECK(outer->after == 1);
    for (uint32_t code = 420; code <= 421; ++code) {
        for (uint32_t id = 0; id <= 1; ++id) {
            outer->panic_code = code;
            outer->source_id = id;
            CHECK(xrt_run_embedded(outer->owner, &outer->failure, panic_root, outer) == XRT_EXECUTION_FAILURE);
            CHECK(outer->failure.kind == XRT_FAILURE_ARITHMETIC_PANIC);
            CHECK(outer->failure.detail.panic.code == code && outer->failure.detail.panic.source_id == id);
            memcpy(prior, &outer->failure, sizeof(prior));
            CHECK(xrt_run_embedded(outer->owner, &outer->failure, normal_root, outer) == XRT_EXECUTION_COMPLETED);
            CHECK(!memcmp(prior, &outer->failure, sizeof(prior)));
        }
    }
    inner->panic_code = 420;
    inner->source_id = 1;
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, nested_root, outer) == XRT_EXECUTION_FAILURE);
    CHECK(outer->inner_status == XRT_EXECUTION_FAILURE);
    CHECK(inner->failure.kind == XRT_FAILURE_ARITHMETIC_PANIC && inner->failure.detail.panic.code == 420);
    CHECK(inner->failure.detail.panic.source_id == 1 && outer->failure.detail.panic.code == 421);
    // A recovered panic restores the enclosing output binding and its variant.
    outer->panic_code = 0;
    SavedOutput saved = disable_output();
    CHECK(xrt_run_embedded(outer->owner, &outer->failure, nested_root, outer) == XRT_EXECUTION_FAILURE);
    restore_output(saved);
    CHECK(outer->failure.kind == XRT_FAILURE_OUTPUT && outer->failure.detail.output.source_id == 1);
    CHECK(inner->failure.kind == XRT_FAILURE_ARITHMETIC_PANIC && outer->inner_status == XRT_EXECUTION_FAILURE);
    xrt_locations_release(inner->owner);
    xrt_locations_release(outer->owner);
    xr_free(inner);
    xr_free(outer);
    fputs("Synchronous runtime scope passed\n", stderr);
    return 0;
}
