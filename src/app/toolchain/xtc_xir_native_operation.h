/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_native_operation.h - One native transaction and its cleanup owner
 */
#ifndef XTC_XIR_NATIVE_OPERATION_H
#define XTC_XIR_NATIVE_OPERATION_H
#include "xtc_xir_invocation.h"
#include "xtc_xir_workspace.h"

typedef struct XtcXirNativeOperation XtcXirNativeOperation;
typedef enum XtcXirNativeOperationStatus {
    XTC_XIR_NATIVE_OK, XTC_XIR_NATIVE_INVALID, XTC_XIR_NATIVE_UNRESOLVED,
    XTC_XIR_NATIVE_UNSUPPORTED, XTC_XIR_NATIVE_BUDGET, XTC_XIR_NATIVE_OUT_OF_MEMORY,
    XTC_XIR_NATIVE_IO, XTC_XIR_NATIVE_TIMEOUT, XTC_XIR_NATIVE_CANCELLED,
    XTC_XIR_NATIVE_CHILD_FAILED, XTC_XIR_NATIVE_REPLAY_MISMATCH,
    XTC_XIR_NATIVE_PENDING
} XtcXirNativeOperationStatus;
typedef enum XtcXirNativeOperationPhase {
    XTC_XIR_NATIVE_NEW, XTC_XIR_NATIVE_RUNNING,
    XTC_XIR_NATIVE_READY, XTC_XIR_NATIVE_FAILED, XTC_XIR_NATIVE_CLOSING
} XtcXirNativeOperationPhase;
typedef enum XtcXirNativeOperationDomain {
    XTC_XIR_NATIVE_SELF, XTC_XIR_NATIVE_RESOURCE, XTC_XIR_NATIVE_WORKSPACE,
    XTC_XIR_NATIVE_SDK, XTC_XIR_NATIVE_FILE_LEASE, XTC_XIR_NATIVE_PROCESS,
    XTC_XIR_NATIVE_INVOCATION, XTC_XIR_NATIVE_PATH
} XtcXirNativeOperationDomain;
typedef struct XtcXirNativeOperationDiagnostic {
    XtcXirNativeOperationStatus status;
    XtcXirNativeOperationDomain domain;
    int code;
    uint32_t os_error;
    XrXirInvocationDiagnostic invocation;
} XtcXirNativeOperationDiagnostic;
typedef struct XtcXirNativeOperationLimits {
    XrXirInvocationLimits invocation;
    uint32_t timeout_ms;
    size_t process_output_bytes;
} XtcXirNativeOperationLimits;
typedef struct XtcXirNativeOperationRequest {
    const XrXirNativeProjection *projection;
    const XrXirRuntimeSdk *sdk;
    const char *compiler, *linker;
    XrXirMsvcRecipe msvc;
    const XrXirInvocationLibrary *libraries;
    uint32_t library_count;
    XrProcessCancelled cancelled;
    void *cancel_context;
} XtcXirNativeOperationRequest;

/* Copies/preallocates without disk I/O. All operations are serial. The single
 * mandatory ledger is retained by owned blocks; no default budget is created.
 * Output must be NULL and is unchanged on failure. */
XR_FUNC XtcXirNativeOperationStatus xtc_xir_native_operation_new(XrCompileResources *resources,
    const XtcXirWorkspaceRequest *workspace, const XtcXirNativeOperationLimits *limits,
    XtcXirNativeOperation **output);
/* Exactly one synchronous transaction. Before any new allocation or disk I/O,
 * reject producers from another ledger. Request pointers are borrowed only
 * during this call. The real SDK launcher and five libraries are resolved through the admitted SDK owner. Provider
 * paths are explicit facts, not discovery or Target execution authority.
 * OK means a successful synchronous invocation and READY owned output.
 * Failures retain the operation for close, including partially created
 * workspace contents. */
XR_FUNC XtcXirNativeOperationStatus xtc_xir_native_operation_run(XtcXirNativeOperation *owner,
    const XtcXirNativeOperationRequest *request);
/* READY-only borrows. Facts remain LOCKED_REPLAY_FACTS, never Target admission. */
XR_FUNC const XrXirInvocationFacts *xtc_xir_native_operation_facts(const XtcXirNativeOperation *owner);
XR_FUNC const XrXirInvocationProviderFacts *xtc_xir_native_operation_provider(const XtcXirNativeOperation *owner);
XR_FUNC const XrXirInvocationFile *xtc_xir_native_operation_file(const XtcXirNativeOperation *owner,
    uint32_t index);
/* Reads the held OUTPUT handle without reopening or hashing. Both outputs must
 * be empty and are preserved on failure. Returned bytes independently retain
 * the ledger and survive operation close; use xr_compile_resources_free. */
XR_FUNC XtcXirNativeOperationStatus xtc_xir_native_operation_read_output(XtcXirNativeOperation *owner,
    uint64_t limit, void **bytes, size_t *length);
XR_FUNC XrCompileResources *xtc_xir_native_operation_resources(const XtcXirNativeOperation *owner);
XR_FUNC XtcXirNativeOperationPhase xtc_xir_native_operation_phase(const XtcXirNativeOperation *owner);
XR_FUNC const XtcXirNativeOperationDiagnostic *xtc_xir_native_operation_diagnostic(const XtcXirNativeOperation *owner);
XR_FUNC const XtcXirNativeOperationDiagnostic *xtc_xir_native_operation_cleanup_diagnostic(const XtcXirNativeOperation *owner);
/* Release invocation/process/launcher leases, then reclaim the private
 * workspace with a finite positive step cap.
 * No new allocation/work is needed. PENDING or errors preserve the owner for
 * retry. The immutable first failure is separate from current cleanup status.
 * OK frees/nulls; NULL is already closed. READY output
 * borrows expire when close starts. Read diagnostics before final close. */
XR_FUNC XtcXirNativeOperationStatus xtc_xir_native_operation_close(XtcXirNativeOperation **owner,
    uint32_t workspace_steps);
#endif // XTC_XIR_NATIVE_OPERATION_H
