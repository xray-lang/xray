/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_workspace.h - Private temporary subtree ownership on one ledger
 */
#ifndef XTC_XIR_WORKSPACE_H
#define XTC_XIR_WORKSPACE_H
#include "../../base/xcompile_resources.h"

typedef struct XtcXirWorkspace XtcXirWorkspace;
typedef enum XtcXirWorkspaceStatus {
    XTC_XIR_WORKSPACE_OK, XTC_XIR_WORKSPACE_INVALID, XTC_XIR_WORKSPACE_UNRESOLVED,
    XTC_XIR_WORKSPACE_UNSUPPORTED, XTC_XIR_WORKSPACE_BUDGET, XTC_XIR_WORKSPACE_OUT_OF_MEMORY,
    XTC_XIR_WORKSPACE_IO, XTC_XIR_WORKSPACE_IDENTITY_MISMATCH, XTC_XIR_WORKSPACE_REPARSE,
    XTC_XIR_WORKSPACE_PENDING
} XtcXirWorkspaceStatus;
typedef struct XtcXirWorkspaceLimits { uint32_t path_bytes, depth, enum_bytes; } XtcXirWorkspaceLimits;
typedef struct XtcXirWorkspaceRequest {
    const char *absolute_parent;
    XtcXirWorkspaceLimits limits;
} XtcXirWorkspaceRequest;
typedef struct XtcXirWorkspacePaths { const char *root, *input, *output; } XtcXirWorkspacePaths;
typedef struct XtcXirWorkspaceDiagnostic { XtcXirWorkspaceStatus status; uint32_t os_error; } XtcXirWorkspaceDiagnostic;

/* Copies and preallocates on the mandatory original ledger without disk I/O.
 * Output must be NULL and remains unchanged on failure. Calls are serial.
 * Limits include the root in depth; enum_bytes must hold one NTFS name. */
XR_FUNC XtcXirWorkspaceStatus xtc_xir_workspace_new(XrCompileResources *resources,
    const XtcXirWorkspaceRequest *request, XtcXirWorkspace **output);
/* Creates root/input/output exactly once. Any failure keeps this owner and
 * every partial creation for close. Local drive-absolute NTFS only. */
XR_FUNC XtcXirWorkspaceStatus xtc_xir_workspace_create(XtcXirWorkspace *owner);
/* Description only: paths can describe a partial failed create. */
XR_FUNC const XtcXirWorkspacePaths *xtc_xir_workspace_paths(const XtcXirWorkspace *owner);
XR_FUNC XrCompileResources *xtc_xir_workspace_resources(const XtcXirWorkspace *owner);
XR_FUNC XtcXirWorkspaceStatus xtc_xir_workspace_status(const XtcXirWorkspace *owner);
XR_FUNC const XtcXirWorkspaceDiagnostic *xtc_xir_workspace_diagnostic(const XtcXirWorkspace *owner);
/* Caller must first complete all processes and release borrowed leases.
 * Deletes ordinary descendants of the original private root, never follows a
 * reparse point, and only unlinks the named entry of a regular hard link.
 * No new heap or ledger work is needed. Each explicit OS call and existing
 * lease-owner release consumes one step; the latter's internal ancestor cleanup
 * is indivisible. A positive finite step_limit is not a real-time deadline.
 * PENDING/errors retain all remaining responsibility for retry. OK frees/nulls;
 * read the sticky first diagnostic before closing. NULL is already closed. */
XR_FUNC XtcXirWorkspaceStatus xtc_xir_workspace_close(XtcXirWorkspace **owner, uint32_t step_limit);
#endif // XTC_XIR_WORKSPACE_H
