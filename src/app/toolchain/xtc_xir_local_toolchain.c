/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_local_toolchain.c - One ledger for discovery and SDK admission
 */
#include "xtc_xir_local_toolchain.h"
#include "xtc_xir_file_lease.h"
#include "../../base/xfileio.h"
#include "../../os/os_fs.h"
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <windows.h>
#include "../../base/xwindows_utf8.h"
#endif

struct XtcXirLocalToolchain {
    XrCompileResources *resources;
    XrOsIoPolicy policy;
    XrXirRuntimeSdk *sdk;
    XtcXirLocalToolchainView view;
    XtcXirLocalDiagnostic diagnostic;
    char *strings[32];
    uint32_t count;
};
static bool local_fail(XtcXirLocalToolchain *owner, XtcXirLocalStatus status,
    XtcXirLocalDomain domain, int code) {
    if (owner->diagnostic.status == XTC_XIR_LOCAL_OK) {
        owner->diagnostic.status = status; owner->diagnostic.domain = domain;
        owner->diagnostic.code = code;
    }
    return false;
}
static XtcXirLocalStatus local_resource_status(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK ? XTC_XIR_LOCAL_OK :
        status == XR_COMPILE_RESOURCE_BUDGET ? XTC_XIR_LOCAL_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XTC_XIR_LOCAL_OUT_OF_MEMORY : XTC_XIR_LOCAL_INVALID;
}
static bool local_work(XtcXirLocalToolchain *owner, uint64_t units) {
    if (owner->diagnostic.status != XTC_XIR_LOCAL_OK) return false;
    XrCompileResourceStatus status = xr_compile_resources_work(owner->resources, units);
    return status == XR_COMPILE_RESOURCE_OK || local_fail(owner, local_resource_status(status), XTC_XIR_LOCAL_RESOURCE, status);
}
static void *local_allocate(XtcXirLocalToolchain *owner, size_t bytes) {
    if (owner->diagnostic.status != XTC_XIR_LOCAL_OK) return NULL;
    void *out = NULL;
    XrCompileResourceStatus status = xr_compile_resources_alloc(owner->resources, bytes, &out);
    if (status != XR_COMPILE_RESOURCE_OK) local_fail(owner, local_resource_status(status), XTC_XIR_LOCAL_RESOURCE, status);
    return out;
}
static bool local_io(XtcXirLocalToolchain *owner, XrOsIoStatus status) {
    if (status == XR_OS_IO_OK) return true;
    XtcXirLocalStatus mapped = status == XR_OS_IO_NOT_FOUND ? XTC_XIR_LOCAL_UNRESOLVED :
        status == XR_OS_IO_BUDGET ? XTC_XIR_LOCAL_BUDGET :
        status == XR_OS_IO_OUT_OF_MEMORY ? XTC_XIR_LOCAL_OUT_OF_MEMORY :
        status == XR_OS_IO_UNSUPPORTED ? XTC_XIR_LOCAL_UNSUPPORTED :
        status == XR_OS_IO_BAD_ARGUMENT ? XTC_XIR_LOCAL_INVALID : XTC_XIR_LOCAL_IO;
    return local_fail(owner, mapped, XTC_XIR_LOCAL_FILESYSTEM, status);
}
static bool local_length(XtcXirLocalToolchain *owner, const char *text, size_t *length) {
    size_t n = 0;
    while (local_work(owner, 1)) {
        if (!text[n]) { *length = n; return true; }
        if (++n > XR_PATH_LIMIT_MAX_PATH) return local_fail(owner, XTC_XIR_LOCAL_BUDGET, XTC_XIR_LOCAL_SELF, 0);
    }
    return false;
}
static char *local_copy(XtcXirLocalToolchain *owner, const char *text, size_t length) {
    char *copy = local_allocate(owner, length + 1);
    if (copy && local_work(owner, length + 1)) { memcpy(copy, text, length); copy[length] = 0; return copy; }
    xr_compile_resources_free(copy); return NULL;
}
static bool local_keep(XtcXirLocalToolchain *owner, char *text, const char **output) {
    if (!text) return false;
    if (owner->count == 32) local_fail(owner, XTC_XIR_LOCAL_BUDGET, XTC_XIR_LOCAL_SELF, 0);
    if (!local_work(owner, 2 * sizeof(text) + sizeof(owner->count))) { xr_compile_resources_free(text); return false; }
    owner->strings[owner->count++] = text; *output = text; return true;
}
static bool local_lease_status(XtcXirLocalToolchain *owner, XrXirTargetStatus status) {
    if (status == XR_XIR_TARGET_OK) return true;
    return local_fail(owner, status == XR_XIR_TARGET_UNRESOLVED ? XTC_XIR_LOCAL_UNRESOLVED :
        status == XR_XIR_TARGET_BUDGET ? XTC_XIR_LOCAL_BUDGET :
        status == XR_XIR_TARGET_OUT_OF_MEMORY ? XTC_XIR_LOCAL_OUT_OF_MEMORY :
        status == XR_XIR_TARGET_UNSUPPORTED ? XTC_XIR_LOCAL_UNSUPPORTED :
        status == XR_XIR_TARGET_IO ? XTC_XIR_LOCAL_IO : XTC_XIR_LOCAL_INVALID,
        XTC_XIR_LOCAL_FILESYSTEM, status);
}
/* Directory admission reuses the existing no-reparse ancestor walker. The
 * locator releases this check lease; the later operation owns execution leases. */
static bool local_path(XtcXirLocalToolchain *owner, const char *path, bool directory, const char **output) {
    char *absolute = NULL, *parent = NULL; XtcXirFileLease *lease = NULL;
    if (!path || !path[0]) return local_fail(owner, XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_SELF, 0);
    bool okay = true;
#ifdef XR_OS_WINDOWS
    wchar_t *wide = NULL; size_t units = 0;
    okay = local_io(owner, xr_win_utf8_owned(&owner->policy, path, &wide, &units));
    if (okay) {
        XrIoContext io = {&owner->policy, XR_OS_IO_OK};
        okay = xr_win_path_components(&io, wide, units);
        if (!okay) local_io(owner, io.status);
    }
    xr_compile_resources_free(wide);
#endif
    if (okay) okay = local_io(owner, xr_realpath_owned(&owner->policy, path, &absolute));
    if (okay && directory) {
        size_t length = 0; okay = local_length(owner, absolute, &length);
        while (okay && length > 3) {
            okay = local_work(owner, 1);
            if (!okay || (absolute[length-1] != '/' && absolute[length-1] != '\\')) break;
            okay = local_work(owner, 1); if (okay) absolute[--length] = 0;
        }
    }
    XrFsStat stat = {0};
    if (okay) okay = local_io(owner, xr_os_io_stat(&owner->policy, absolute, &stat));
    if (okay && stat.kind != (directory ? XR_FS_DIR : XR_FS_FILE))
        okay = local_fail(owner, XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_FILESYSTEM, XR_OS_IO_BAD_ARGUMENT);
    if (okay && !directory) okay = local_io(owner, xr_path_dirname_owned(&owner->policy, absolute, &parent));
    if (okay) okay = local_lease_status(owner, xtc_xir_file_lease_directory_open(owner->resources,
        directory ? absolute : parent, &lease));
    xtc_xir_file_lease_free(lease); xr_compile_resources_free(parent);
    if (okay) return local_keep(owner, absolute, output);
    xr_compile_resources_free(absolute); return false;
}
static bool local_join_path(XtcXirLocalToolchain *owner, const char *root, const char *suffix,
    bool directory, const char **output) {
    char *joined = NULL;
    if (!local_io(owner, xr_path_join_owned(&owner->policy, root, suffix, &joined))) return false;
    bool okay = local_path(owner, joined, directory, output); xr_compile_resources_free(joined); return okay;
}
static bool local_process(XtcXirLocalToolchain *owner, const XrProcessSpec *spec, XrProcessResult *output) {
    XrToolchainProcess *process = NULL;
    XrProcessStatus status = xtc_process_prepare(owner->resources, spec, &process);
    if (status == XTC_PROCESS_OK) status = xtc_process_run(process, NULL, NULL, output);
    xtc_process_free(process);
    if (status != XTC_PROCESS_OK) {
        XtcXirLocalStatus mapped = status == XTC_PROCESS_BUDGET ? XTC_XIR_LOCAL_BUDGET :
            status == XTC_PROCESS_OUT_OF_MEMORY ? XTC_XIR_LOCAL_OUT_OF_MEMORY :
            status == XTC_PROCESS_TIMEOUT ? XTC_XIR_LOCAL_TIMEOUT :
            status == XTC_PROCESS_UNRESOLVED ? XTC_XIR_LOCAL_UNRESOLVED :
            status == XTC_PROCESS_UNSUPPORTED ? XTC_XIR_LOCAL_UNSUPPORTED :
            status == XTC_PROCESS_INVALID ? XTC_XIR_LOCAL_INVALID : XTC_XIR_LOCAL_IO;
        return local_fail(owner, mapped, XTC_XIR_LOCAL_PROCESS, status);
    }
    if (output->stdout_bytes.truncated || output->stderr_bytes.truncated)
        return local_fail(owner, XTC_XIR_LOCAL_BUDGET, XTC_XIR_LOCAL_PROCESS, XTC_PROCESS_BUDGET);
    if (output->exit_code) {
        owner->diagnostic.child_exit = output->exit_code;
        return local_fail(owner, XTC_XIR_LOCAL_CHILD_FAILED, XTC_XIR_LOCAL_PROCESS, output->exit_code);
    }
    return true;
}
#ifdef XR_OS_WINDOWS
#include "xtc_xir_local_toolchain_windows.inc.c"
#endif

static bool local_sdk_load(XtcXirLocalToolchain *owner, const char *root, size_t limit) {
    const char *checked = NULL; char *manifest_path = NULL; uint8_t *manifest = NULL; size_t bytes = 0;
    bool okay = local_path(owner, root, true, &checked) &&
        local_io(owner, xr_path_join_owned(&owner->policy, checked, "sdk_manifest.json", &manifest_path)) &&
        local_io(owner, xr_os_io_read_regular_file(&owner->policy, manifest_path, limit, &manifest, &bytes));
    if (okay) {
        XrXirRuntimeSdkRequest request = {checked, manifest, bytes, owner->resources};
        XrXirRuntimeSdkStatus status = xr_xir_runtime_sdk_load(&request, &owner->sdk);
        if (status != XR_XIR_SDK_OK) okay = local_fail(owner,
            status == XR_XIR_SDK_BUDGET ? XTC_XIR_LOCAL_BUDGET :
            status == XR_XIR_SDK_OUT_OF_MEMORY ? XTC_XIR_LOCAL_OUT_OF_MEMORY :
            status == XR_XIR_SDK_IO ? XTC_XIR_LOCAL_IO :
            status == XR_XIR_SDK_UNRESOLVED ? XTC_XIR_LOCAL_UNRESOLVED :
            status == XR_XIR_SDK_UNSUPPORTED ? XTC_XIR_LOCAL_UNSUPPORTED : XTC_XIR_LOCAL_INVALID,
            XTC_XIR_LOCAL_RUNTIME_SDK, status);
    }
    xr_compile_resources_free(manifest); xr_compile_resources_free(manifest_path); return okay;
}
static bool local_sdk_select(XtcXirLocalToolchain *owner, const char *explicit_root, size_t limit) {
    if (explicit_root) return local_sdk_load(owner, explicit_root, limit);
    char *executable = NULL, *directory = NULL, *candidate = NULL;
    bool okay = local_io(owner, xr_os_io_self_exe_path(&owner->policy, &executable)) &&
        local_io(owner, xr_path_dirname_owned(&owner->policy, executable, &directory));
    const char *suffixes[2] = {"xir-runtime-sdk", "../lib/xray/xir-runtime-sdk"};
    bool found = false;
    for (unsigned i = 0; okay && !found && i < 2; ++i) {
        okay = local_io(owner, xr_path_join_owned(&owner->policy, directory, suffixes[i], &candidate));
        XrFsStat stat;
        if (okay) {
            XrOsIoStatus status = xr_os_io_stat(&owner->policy, candidate, &stat);
            if (status != XR_OS_IO_NOT_FOUND) { okay = local_io(owner, status); found = okay; }
        }
        if (found) okay = local_sdk_load(owner, candidate, limit);
        xr_compile_resources_free(candidate); candidate = NULL;
    }
    xr_compile_resources_free(executable); xr_compile_resources_free(directory);
    if (okay && !found) okay = local_fail(owner, XTC_XIR_LOCAL_UNRESOLVED, XTC_XIR_LOCAL_FILESYSTEM, XR_OS_IO_NOT_FOUND);
    return okay;
}
XR_FUNC XtcXirLocalStatus xtc_xir_local_toolchain_open(XrCompileResources *resources,
    const XtcXirLocalToolchainRequest *request, XtcXirLocalToolchain **output, XtcXirLocalDiagnostic *diagnostic) {
    XtcXirLocalDiagnostic initial = {XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_REQUEST, XTC_XIR_LOCAL_SELF, 0, 0, 0};
    if (!resources || !request || !output || *output || !request->timeout_ms ||
        !request->output_limit || request->output_limit == SIZE_MAX || !request->manifest_limit || request->manifest_limit == SIZE_MAX) {
        if (diagnostic) *diagnostic = initial;
        return initial.status;
    }
#if !defined(XR_OS_WINDOWS) || !defined(_M_X64) && !defined(__x86_64__)
    initial.status = XTC_XIR_LOCAL_UNSUPPORTED;
    if (diagnostic) *diagnostic = initial;
    return initial.status;
#else
    XtcXirLocalToolchain *owner = NULL;
    XrCompileResourceStatus allocation = xr_compile_resources_calloc(resources, 1, sizeof(*owner), (void **)&owner);
    if (allocation != XR_COMPILE_RESOURCE_OK) {
        initial.status = local_resource_status(allocation); initial.domain = XTC_XIR_LOCAL_RESOURCE; initial.code = allocation;
        if (diagnostic) *diagnostic = initial;
        return initial.status;
    }
    owner->resources = resources; owner->policy = xr_compile_io_policy(resources);
    owner->diagnostic.stage = XTC_XIR_LOCAL_WORKSPACE;
    bool okay = local_workspace(owner, request->workspace_parent);
    if (okay) { owner->diagnostic.stage = XTC_XIR_LOCAL_DISCOVERY; okay = local_discover(owner, request); }
    if (okay) { owner->diagnostic.stage = XTC_XIR_LOCAL_SDK; okay = local_sdk_select(owner, request->sdk_root, request->manifest_limit); }
    XtcXirLocalStatus status = owner->diagnostic.status;
    if (diagnostic) *diagnostic = owner->diagnostic;
    if (okay) *output = owner;
    else xtc_xir_local_toolchain_free(owner);
    return status;
#endif
}
XR_FUNC const XtcXirLocalToolchainView *xtc_xir_local_toolchain_view(const XtcXirLocalToolchain *owner) {
    return owner ? &owner->view : NULL;
}
XR_FUNC const XrXirRuntimeSdk *xtc_xir_local_toolchain_sdk(const XtcXirLocalToolchain *owner) { return owner ? owner->sdk : NULL; }
XR_FUNC XrCompileResources *xtc_xir_local_toolchain_resources(const XtcXirLocalToolchain *owner) { return owner ? owner->resources : NULL; }
XR_FUNC void xtc_xir_local_toolchain_free(XtcXirLocalToolchain *owner) {
    if (!owner) return;
    xr_xir_runtime_sdk_free(owner->sdk);
    for (uint32_t i = 0; i < owner->count; ++i) xr_compile_resources_free(owner->strings[i]);
    xr_compile_resources_free(owner);
}
