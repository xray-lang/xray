/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_build_native.inc.c - Admit native bytes before output publication
 */
static int build_operation_exit(XtcXirNativeOperationStatus status) {
    return status == XTC_XIR_NATIVE_OUT_OF_MEMORY || status == XTC_XIR_NATIVE_INVALID ?
        XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL;
}
static int build_operation_close(XtcXirNativeOperation **owner) {
    bool had_failure = false;
    XtcXirNativeOperationDiagnostic first = {0}, last = {0};
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        XtcXirNativeOperationStatus status = xtc_xir_native_operation_close(owner, 1024);
        if (status == XTC_XIR_NATIVE_OK) {
            if (had_failure) fprintf(stderr,
                "XR_BUILD_6008: native cleanup recovered (status=%u code=%d os=%u published=0)\n",
                (unsigned)first.status, first.code, first.os_error);
            return had_failure ? build_operation_exit(first.status) : XR_CLI_EXIT_OK;
        }
        last = *xtc_xir_native_operation_cleanup_diagnostic(*owner);
        if (status != XTC_XIR_NATIVE_PENDING && !had_failure) {
            first = last; had_failure = true;
        }
    }
    fprintf(stderr,
        "XR_BUILD_6008: terminal native cleanup failure (status=%u code=%d os=%u cleanup_pending=1 published=0)\n",
        (unsigned)last.status, last.code, last.os_error);
    fflush(stdout); fflush(stderr);
    /* A command must not return while abandoning a private workspace owner. */
    _Exit(XR_CLI_EXIT_INTERNAL);
}
static int build_native_prepare(XrCompileResources *resources, const XrCliInvocation *inv,
    const XrXirNativeProjection *projection, XrXirNativeArtifact **artifact) {
    XtcXirLocalToolchainRequest locate = {xr_cli_opt_string(&inv->options, "cc", NULL),
        NULL, NULL, 30000, 4 * 1024 * 1024, 1024 * 1024};
    XtcXirLocalDiagnostic local_diagnostic = {0};
    XtcXirLocalToolchain *local = NULL;
    XtcXirLocalStatus found = xtc_xir_local_toolchain_open(resources, &locate, &local, &local_diagnostic);
    if (found != XTC_XIR_LOCAL_OK) {
        fprintf(stderr,
            "XR_BUILD_6006: native toolchain lookup failed (status=%u stage=%u domain=%u code=%d os=%u child=%d)\n",
            (unsigned)found, (unsigned)local_diagnostic.stage, (unsigned)local_diagnostic.domain,
            local_diagnostic.code, local_diagnostic.os_error, local_diagnostic.child_exit);
        return found == XTC_XIR_LOCAL_OUT_OF_MEMORY ? XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL;
    }
    const XtcXirLocalToolchainView *view = xtc_xir_local_toolchain_view(local);
    XtcXirWorkspaceRequest workspace = {view->workspace_parent, {XR_PATH_LIMIT_MAX_PATH, 32, 65536}};
    XtcXirNativeOperationLimits limits = {{{4 * 1024 * 1024, XR_PATH_LIMIT_MAX_PATH, 4096},
        BUILD_C_LIMIT, 4096}, 30000, 4 * 1024 * 1024};
    XtcXirNativeOperation *operation = NULL;
    XtcXirNativeOperationStatus status = xtc_xir_native_operation_new(resources, &workspace, &limits, &operation);
    if (status == XTC_XIR_NATIVE_OK) {
        XtcXirNativeOperationRequest request = {projection, xtc_xir_local_toolchain_sdk(local),
            view->compiler, view->linker, view->msvc, view->libraries, view->library_count, NULL, NULL};
        status = xtc_xir_native_operation_run(operation, &request);
    }
    int result = XR_CLI_EXIT_OK;
    if (status != XTC_XIR_NATIVE_OK) {
        const XtcXirNativeOperationDiagnostic *d = xtc_xir_native_operation_diagnostic(operation);
        fprintf(stderr,
            "XR_BUILD_6007: native transaction failed (status=%u domain=%u code=%d os=%u stage=%u pass=%u child=%d)\n",
            (unsigned)status, d ? (unsigned)d->domain : 0, d ? d->code : 0, d ? d->os_error : 0,
            d ? (unsigned)d->invocation.stage : 0, d ? (unsigned)d->invocation.pass : 0,
            d ? d->invocation.exit_code : 0);
        result = build_operation_exit(status);
    } else {
        XtcXirNativeAdmissionDiagnostic diagnostic = {0};
        XtcXirNativeAdmissionStatus admitted = xtc_xir_native_admit(operation, projection,
            xtc_xir_local_toolchain_sdk(local), BUILD_C_LIMIT, artifact, &diagnostic);
        if (admitted != XTC_XIR_ADMISSION_OK) {
            fprintf(stderr, "XR_BUILD_6009: native admission failed (status=%u domain=%u code=%d stage=%u)\n",
                (unsigned)admitted, (unsigned)diagnostic.domain, diagnostic.code, (unsigned)diagnostic.stage);
            result = admitted == XTC_XIR_ADMISSION_OUT_OF_MEMORY ? XR_CLI_EXIT_INTERNAL : XR_CLI_EXIT_FAIL;
        }
    }
    xtc_xir_local_toolchain_free(local);
    int closed = build_operation_close(&operation);
    if (closed > result) result = closed;
    if (result != XR_CLI_EXIT_OK) {
        xr_compile_native_artifact_free(*artifact); *artifact = NULL;
    }
    return result;
}
