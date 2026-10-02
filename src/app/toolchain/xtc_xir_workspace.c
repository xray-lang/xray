/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_workspace.c - Owned private directories with allocation-free cleanup
 *
 * KEY CONCEPT:
 *   Disk side effects start only after a recoverable owner has been published.
 */
#include "xtc_xir_workspace.h"
#include "xtc_xir_file_lease.h"
#include "../../shared/xr_utf8_core.h"
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <windows.h>
#include <winternl.h>
#include <bcrypt.h>

typedef struct WorkspaceNode {
    HANDLE created, anchor, deletion;
    XtcXirFileLease *lease;
    FILE_ID_INFO identity;
    wchar_t name[40];
    USHORT name_bytes;
    bool exists, identified, anchored;
} WorkspaceNode;
typedef struct WorkspaceFrame {
    HANDLE handle;
    FILE_ID_INFO identity;
    wchar_t name[256];
    USHORT name_bytes;
    uint8_t state;
    bool directory, restart;
    int fixed;
    XtcXirWorkspaceStatus rejection;
} WorkspaceFrame;
#endif
struct XtcXirWorkspace {
    XrCompileResources *resources;
    XtcXirWorkspaceLimits limits;
    XtcXirWorkspaceDiagnostic diagnostic;
    XtcXirWorkspacePaths paths;
    char *parent, *text;
    void *enumeration;
    bool attempted, closing;
#ifdef XR_OS_WINDOWS
    XtcXirFileLease *parent_lease;
    HANDLE parent_handle;
    WorkspaceNode nodes[3];
    WorkspaceFrame *frames;
    uint32_t close_phase, close_index, close_substage, frame_count;
#endif
};
static XtcXirWorkspaceStatus workspace_fail(XtcXirWorkspace *owner, XtcXirWorkspaceStatus status, uint32_t error) {
    if (owner->diagnostic.status == XTC_XIR_WORKSPACE_OK)
        owner->diagnostic = (XtcXirWorkspaceDiagnostic){status, error};
    return status;
}
static XtcXirWorkspaceStatus workspace_resource_status(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK ? XTC_XIR_WORKSPACE_OK :
        status == XR_COMPILE_RESOURCE_BUDGET ? XTC_XIR_WORKSPACE_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XTC_XIR_WORKSPACE_OUT_OF_MEMORY : XTC_XIR_WORKSPACE_INVALID;
}
static bool workspace_resource(XtcXirWorkspace *owner, XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return true;
    workspace_fail(owner, workspace_resource_status(status), 0); return false;
}
static bool workspace_work(XtcXirWorkspace *owner, uint64_t units) {
    return workspace_resource(owner, xr_compile_resources_work(owner->resources, units));
}
static int workspace_utf8_read(void *context, const uint8_t *address, uint8_t *out) {
    if (!workspace_work(context, 1)) return 0;
    *out = *address; return 1;
}
static bool workspace_length(XtcXirWorkspace *owner, const char *text, size_t *length) {
    for (size_t i = 0;; ++i) {
        if (!workspace_work(owner, 1)) return false;
        if (!text[i]) { *length = i; return true; }
        if (i == owner->limits.path_bytes) { workspace_fail(owner, XTC_XIR_WORKSPACE_BUDGET, 0); return false; }
    }
}
static void workspace_free(XtcXirWorkspace *owner) {
#ifdef XR_OS_WINDOWS
    xr_compile_resources_free(owner->frames);
#endif
    xr_compile_resources_free(owner->enumeration);
    xr_compile_resources_free(owner->parent); xr_compile_resources_free(owner->text);
    xr_compile_resources_free(owner);
}
XR_FUNC XtcXirWorkspaceStatus xtc_xir_workspace_new(XrCompileResources *resources,
    const XtcXirWorkspaceRequest *request, XtcXirWorkspace **output) {
    if (!resources || !request || !request->absolute_parent || !output || *output)
        return XTC_XIR_WORKSPACE_INVALID;
    if (request->limits.depth < 2 || request->limits.enum_bytes < 1024 || !request->limits.path_bytes ||
        request->limits.path_bytes > 32767 || request->limits.depth > 32767)
        return XTC_XIR_WORKSPACE_BUDGET;
    XtcXirWorkspace *owner = NULL;
    XrCompileResourceStatus status = xr_compile_resources_calloc(resources, 1, sizeof(*owner), (void **)&owner);
    if (status != XR_COMPILE_RESOURCE_OK) return workspace_resource_status(status);
    owner->resources = resources;
    if (!workspace_work(owner, sizeof(request->limits))) goto failed;
    owner->limits = request->limits;
    size_t length = 0;
    if (!workspace_length(owner, request->absolute_parent, &length)) goto failed;
    if (length < 3 || !workspace_work(owner, 3)) goto invalid;
    const char *p = request->absolute_parent;
    if (!((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) || p[1] != ':' ||
        (p[2] != '/' && p[2] != '\\')) goto invalid;
    XrUtf8ScanResult scan;
    if (!xr_utf8_core_scan_strict_read((const uint8_t *)p, length, workspace_utf8_read, owner, &scan)) goto failed;
    if (scan.error != XR_UTF8_OK) goto invalid;
    if (!workspace_resource(owner, xr_compile_resources_alloc(resources, length + 1, (void **)&owner->parent)) ||
        !workspace_work(owner, length + 1)) goto failed;
    memcpy(owner->parent, p, length + 1);
    if (!workspace_resource(owner, xr_compile_resources_calloc(resources, 3,
        (size_t)owner->limits.path_bytes + 1, (void **)&owner->text)) ||
        !workspace_resource(owner, xr_compile_resources_alloc(resources, owner->limits.enum_bytes, &owner->enumeration))) goto failed;
#ifdef XR_OS_WINDOWS
    if (!workspace_resource(owner, xr_compile_resources_calloc(resources, (size_t)owner->limits.depth + 1,
        sizeof(*owner->frames), (void **)&owner->frames))) goto failed;
#endif
    if (!workspace_work(owner, sizeof(*output))) goto failed;
    *output = owner; return XTC_XIR_WORKSPACE_OK;
invalid:
    workspace_fail(owner, XTC_XIR_WORKSPACE_INVALID, 0);
failed:
    {
        XtcXirWorkspaceStatus failure = owner->diagnostic.status;
        workspace_free(owner); return failure;
    }
}
#ifdef XR_OS_WINDOWS
#include "xtc_xir_workspace_windows.inc.c"
#endif
XR_FUNC XtcXirWorkspaceStatus xtc_xir_workspace_create(XtcXirWorkspace *owner) {
    if (!owner || owner->attempted || owner->closing) return XTC_XIR_WORKSPACE_INVALID;
    owner->attempted = true;
#ifdef XR_OS_WINDOWS
    return workspace_create_windows(owner);
#else
    return workspace_fail(owner, XTC_XIR_WORKSPACE_UNSUPPORTED, 0);
#endif
}
XR_FUNC XtcXirWorkspaceStatus xtc_xir_workspace_close(XtcXirWorkspace **slot, uint32_t step_limit) {
    if (!slot || !step_limit) return XTC_XIR_WORKSPACE_INVALID;
    XtcXirWorkspace *owner = *slot;
    if (!owner) return XTC_XIR_WORKSPACE_OK;
    owner->closing = true;
#ifdef XR_OS_WINDOWS
    XtcXirWorkspaceStatus status = workspace_close_windows(owner, step_limit);
    if (status != XTC_XIR_WORKSPACE_OK) return status;
#endif
    workspace_free(owner); *slot = NULL; return XTC_XIR_WORKSPACE_OK;
}
XR_FUNC const XtcXirWorkspacePaths *xtc_xir_workspace_paths(const XtcXirWorkspace *owner) { return owner ? &owner->paths : NULL; }
XR_FUNC XrCompileResources *xtc_xir_workspace_resources(const XtcXirWorkspace *owner) { return owner ? owner->resources : NULL; }
XR_FUNC XtcXirWorkspaceStatus xtc_xir_workspace_status(const XtcXirWorkspace *owner) {
    return owner ? owner->diagnostic.status : XTC_XIR_WORKSPACE_INVALID;
}
XR_FUNC const XtcXirWorkspaceDiagnostic *xtc_xir_workspace_diagnostic(const XtcXirWorkspace *owner) {
    return owner ? &owner->diagnostic : NULL;
}
