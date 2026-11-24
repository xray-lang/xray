/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_host_remove_control.c - Exercise removal under a synchronous host request.
 */
#include "base/xplatform.h"
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_values.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#else
#include <errno.h>
#include <sys/stat.h>
#endif

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "host removal control:%d: %s\n", __LINE__, #expr); \
        exit(2); \
    } \
} while (0)

typedef enum CancelMode { KEEP_CURRENT, CANCEL_BEFORE, CANCEL_AFTER } CancelMode;

typedef struct HostRequest {
    XrtHostExecutionControl control;
    CancelMode mode;
    bool directory;
    bool observed_before;
    bool observed_after;
    bool replaced;
} HostRequest;

static bool leaf_exists(void) {
#if defined(XR_OS_WINDOWS)
    if (GetFileAttributesW(L"leaf") != INVALID_FILE_ATTRIBUTES) return true;
    CHECK(GetLastError() == ERROR_FILE_NOT_FOUND);
#else
    struct stat information;
    if (stat("leaf", &information) == 0) return true;
    CHECK(errno == ENOENT);
#endif
    return false;
}

static void replace_leaf(bool directory) {
    if (directory) {
        /* An empty replacement would be deleted by any replay of rmdir. */
#if defined(XR_OS_WINDOWS)
        CHECK(CreateDirectoryW(L"leaf", NULL));
#else
        CHECK(mkdir("leaf", 0700) == 0);
#endif
    } else {
        FILE *file = fopen("leaf", "wb");
        CHECK(file);
        CHECK(fputs("replacement", file) >= 0);
        CHECK(fclose(file) == 0);
    }
}

static bool request_current(void *context) {
    HostRequest *request = context;
    CHECK(xrt_execution_host_control() == &request->control);
    bool present = leaf_exists();
    request->observed_before |= present;
    request->observed_after |= !present;
    if (!present && request->mode == CANCEL_AFTER) {
        CHECK(!request->replaced);
        replace_leaf(request->directory);
        request->replaced = true;
    }
    return request->mode == KEEP_CURRENT || (request->mode == CANCEL_AFTER && present);
}

#if defined(XR_TEST_NATIVE_REMOVE)
#define main generated_main
#include "generated.c"
#undef main

typedef struct NativeInvocation {
    XrtModuleState *state;
    int64_t result;
} NativeInvocation;

static void invoke_native(void *context) {
    NativeInvocation *invocation = context;
    xrl_invoke(&invocation->result, invocation->state);
}

static XrXirValueStatus run_program(const char *path, const char *stdlib_path, HostRequest *request) {
    (void)path;
    (void)stdlib_path;
    XrtValueRoot root = {0};
    XrtBillingOwner *billing = NULL;
    XrtModuleLayout *layout = NULL;
    NativeInvocation invocation = {.result = 1};
    CHECK(xrt_native_values_prepare(&root, &billing) == XRT_MANAGED_OK);
    CHECK(xrt_module_layout_new(billing, &xrl_state_spec, (XrtManagedLease){0}, &layout) == XRT_MANAGED_OK);
    CHECK(xrt_module_state_new(layout, billing, &invocation.state) == XRT_MANAGED_OK);
    xrt_module_layout_drop(&layout);
    xrt_native_values_use_state(&root, invocation.state, xrl_state_spec.values);
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(xrt_standalone_source_map.locations, xrt_standalone_source_map.count, &locations) ==
        XRT_LOCATION_OK);
    XrtExecutionFailure failure = {0};
    XrtExecutionStatus status = xrt_run_embedded_owned_controlled(locations, &failure, invoke_native, &invocation,
        &root, &request->control);
    CHECK(xrt_value_root_empty(&root));
    xrt_module_state_drop(&invocation.state);
    XrXirValueStatus result = XR_XIR_VALUE_OK;
    if (status == XRT_EXECUTION_COMPLETED) {
        CHECK(invocation.result == 0);
    } else {
        CHECK(status == XRT_EXECUTION_FAILURE && failure.kind == XRT_FAILURE_VALUE && invocation.result == 1);
        result = failure.detail.value.status;
        xrt_value_failure_report(result, xrt_locations_get(locations, failure.detail.value.source_id));
    }
    xrt_execution_failure_drop(&failure);
    xrt_locations_release(locations);
    return result;
}
#else
#include "lir/xlir_source.h"
#include "vm/register/xrbc_lir.h"

static XrXirValueStatus run_program(const char *path, const char *stdlib_path, HostRequest *request) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority = {0};
    char *root = NULL;
    CHECK(xr_compile_module_identity_script_authority_from_source(resources, path, &authority, &root) == XR_MODULE_OK);
    const XrLirSourceRequest source = {resources, path, stdlib_path, false, &authority, NULL, XR_XIR_HOST_TARGET};
    XrLirSourceFailure failure = {0};
    XrLirLowered lowered = {0};
    CHECK(xr_lir_compile_source(&source, &lowered, &failure) == XR_LIR_SOURCE_OK);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(lowered.module, lowered.entry, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_source_failure_free(&failure);
    xr_lir_lowered_free(&lowered);
    xr_compile_resources_free(root);
    xr_compile_resources_release(resources);

    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run_controlled(execution, &request->control);
    CHECK(outcome);
    uint64_t bits = 1;
    XrXirValueStatus result = XR_XIR_VALUE_OK;
    if (xr_rbc_outcome_status(outcome) == XR_RBC_OK) {
        CHECK(xr_rbc_outcome_bits(outcome, &bits) && bits == 0);
    } else {
        uint32_t source_id = 0;
        const XrtLocationOwner *locations = NULL;
        CHECK(!xr_rbc_outcome_bits(outcome, &bits));
        CHECK(xr_rbc_outcome_value_failure(outcome, &result, &source_id, &locations));
        xrt_value_failure_report(result, xrt_locations_get(locations, source_id));
    }
    CHECK(xr_rbc_execution_free(&execution) == XR_OS_IO_OK && !execution);
    xr_rbc_outcome_free(&outcome);
    xr_rbc_program_destroy(program);
    return result;
}
#endif

int main(int argc, char **argv) {
    CHECK(argc == 5);
    CHECK(strcmp(argv[3], "run") == 0 || strcmp(argv[3], "before") == 0 || strcmp(argv[3], "after") == 0);
    CHECK(strcmp(argv[4], "remove") == 0 || strcmp(argv[4], "rmdir") == 0);
    HostRequest request = {
        .mode = strcmp(argv[3], "before") == 0 ? CANCEL_BEFORE :
            strcmp(argv[3], "after") == 0 ? CANCEL_AFTER : KEEP_CURRENT,
        .directory = strcmp(argv[4], "rmdir") == 0,
    };
    request.control = (XrtHostExecutionControl){request_current, &request};
    XrXirValueStatus status = run_program(argv[1], argv[2], &request);
    CHECK(!xrt_execution_host_control() && !xrt_value_root_active());
    CHECK(request.observed_before);
    CHECK(request.observed_after == (request.mode != CANCEL_BEFORE));
    CHECK(request.replaced == (request.mode == CANCEL_AFTER));
    CHECK(status == (request.mode == KEEP_CURRENT ? XR_XIR_VALUE_OK : XR_XIR_VALUE_CANCELLED));
    return request.mode == KEEP_CURRENT ? 0 : 1;
}
