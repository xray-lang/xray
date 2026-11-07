/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_host_toolchain.c - Owned discovery of the host C compiler
 */
#include "xtc_host_toolchain.h"
#include "xtc_xir_file_lease.h"
#include "../../base/xfileio.h"
#include "../../os/os_fs.h"
#include "../../base/xchecks.h"
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <windows.h>
#include "../../base/xwindows_utf8.h"
#else
#include <stdio.h>
#endif

struct XtcHostToolchain {
    XrCompileResources *resources;
    XrOsIoPolicy policy;
    XtcHostCrt crt;
    XtcHostToolchainView view;
    XtcHostDiagnostic diagnostic;
    char *strings[64];
    uint32_t count;
};
static bool local_fail(XtcHostToolchain *owner, XtcHostStatus status,
    XtcHostDomain domain, int code) {
    if (owner->diagnostic.status == XTC_HOST_OK) {
        owner->diagnostic.status = status; owner->diagnostic.domain = domain;
        owner->diagnostic.code = code;
    }
    return false;
}
static XtcHostStatus local_resource_status(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK ? XTC_HOST_OK :
        status == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XTC_HOST_MEMORY_LIMIT :
        status == XR_COMPILE_RESOURCE_OVERFLOW ? XTC_HOST_OVERFLOW :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XTC_HOST_OUT_OF_MEMORY : XTC_HOST_INVALID;
}
static void *local_allocate(XtcHostToolchain *owner, size_t bytes) {
    if (owner->diagnostic.status != XTC_HOST_OK) return NULL;
    void *out = NULL;
    XrCompileResourceStatus status = xr_compile_resources_alloc(owner->resources, bytes, &out);
    if (status != XR_COMPILE_RESOURCE_OK) local_fail(owner, local_resource_status(status), XTC_HOST_RESOURCE, status);
    return out;
}
static bool local_io(XtcHostToolchain *owner, XrOsIoStatus status) {
    if (status == XR_OS_IO_OK) return true;
    XtcHostStatus mapped = status == XR_OS_IO_NOT_FOUND ? XTC_HOST_UNRESOLVED :
        status == XR_OS_IO_MEMORY_LIMIT ? XTC_HOST_MEMORY_LIMIT :
        status == XR_OS_IO_OVERFLOW ? XTC_HOST_OVERFLOW :
        status == XR_OS_IO_OUT_OF_MEMORY ? XTC_HOST_OUT_OF_MEMORY :
        status == XR_OS_IO_UNSUPPORTED ? XTC_HOST_UNSUPPORTED :
        status == XR_OS_IO_BAD_ARGUMENT ? XTC_HOST_INVALID : XTC_HOST_IO;
    return local_fail(owner, mapped, XTC_HOST_FILESYSTEM, status);
}
static bool local_length(XtcHostToolchain *owner, const char *text, size_t *length) {
    size_t n = 0;
    while (owner->diagnostic.status == XTC_HOST_OK) {
        if (!text[n]) { *length = n; return true; }
        if (++n > XR_PATH_LIMIT_MAX_PATH) return local_fail(owner, XTC_HOST_BUDGET, XTC_HOST_SELF, 0);
    }
    return false;
}
#if defined(XR_OS_WINDOWS) || defined(XR_OS_MACOS)
static char *local_copy(XtcHostToolchain *owner, const char *text, size_t length) {
    char *copy = local_allocate(owner, length + 1);
    if (copy) { memcpy(copy, text, length); copy[length] = 0; return copy; }
    xr_compile_resources_free(copy); return NULL;
}
#endif
static bool local_keep(XtcHostToolchain *owner, char *text, const char **output) {
    if (!text) return false;
    if (owner->diagnostic.status != XTC_HOST_OK) {
        xr_compile_resources_free(text); return false;
    }
    if (owner->count >= sizeof(owner->strings) / sizeof(owner->strings[0])) {
        local_fail(owner, XTC_HOST_BUDGET, XTC_HOST_SELF, 0);
        xr_compile_resources_free(text); return false;
    }

    owner->strings[owner->count++] = text; *output = text; return true;
}
static bool local_lease_status(XtcHostToolchain *owner, XrXirTargetStatus status) {
    if (status == XR_XIR_TARGET_OK) return true;
    return local_fail(owner, status == XR_XIR_TARGET_UNRESOLVED ? XTC_HOST_UNRESOLVED :
        status == XR_XIR_TARGET_BUDGET ? XTC_HOST_BUDGET :
        status == XR_XIR_TARGET_OUT_OF_MEMORY ? XTC_HOST_OUT_OF_MEMORY :
        status == XR_XIR_TARGET_MEMORY_LIMIT ? XTC_HOST_MEMORY_LIMIT :
        status == XR_XIR_TARGET_OVERFLOW ? XTC_HOST_OVERFLOW :
        status == XR_XIR_TARGET_UNSUPPORTED ? XTC_HOST_UNSUPPORTED :
        status == XR_XIR_TARGET_IO ? XTC_HOST_IO : XTC_HOST_INVALID,
        XTC_HOST_FILESYSTEM, status);
}
/* Directory admission reuses the existing no-reparse ancestor walker. The
 * locator releases this check lease; the later operation owns execution leases. */
static bool local_path_receive(XtcHostToolchain *owner, const char *path, bool directory, char **output) {
    char *absolute = NULL, *parent = NULL; XtcXirFileLease *lease = NULL;
    if (!path || !path[0]) return local_fail(owner, XTC_HOST_INVALID, XTC_HOST_SELF, 0);
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

            if (!okay || (absolute[length-1] != '/' && absolute[length-1] != '\\')) break;
            absolute[--length] = 0;
        }
    }
    XrFsStat stat = {0};
    if (okay) okay = local_io(owner, xr_os_io_stat(&owner->policy, absolute, &stat));
    if (okay && stat.kind != (directory ? XR_FS_DIR : XR_FS_FILE))
        okay = local_fail(owner, XTC_HOST_INVALID, XTC_HOST_FILESYSTEM, XR_OS_IO_BAD_ARGUMENT);
    if (okay && !directory) okay = local_io(owner, xr_path_dirname_owned(&owner->policy, absolute, &parent));
    if (okay) okay = local_lease_status(owner, xtc_xir_file_lease_directory_open(owner->resources,
        directory ? absolute : parent, &lease));
    xtc_xir_file_lease_free(lease); xr_compile_resources_free(parent);
    if (okay) { *output=absolute; return true; }
    xr_compile_resources_free(absolute); return false;
}
static bool local_path(XtcHostToolchain *owner, const char *path, bool directory, const char **output) {
    char *absolute=NULL;
    return local_path_receive(owner,path,directory,&absolute) && local_keep(owner,absolute,output);
}
#ifdef XR_OS_WINDOWS
static bool local_join_path(XtcHostToolchain *owner, const char *root, const char *suffix,
    bool directory, const char **output) {
    char *joined = NULL;
    if (!local_io(owner, xr_path_join_owned(&owner->policy, root, suffix, &joined))) return false;
    bool okay = local_path(owner, joined, directory, output); xr_compile_resources_free(joined); return okay;
}
#endif
#if defined(XR_OS_WINDOWS) || defined(XR_OS_MACOS)
static bool local_process_status(XtcHostToolchain *owner, XrProcessStatus status) {
    if (status != XTC_PROCESS_OK) {
        XtcHostStatus mapped = status == XTC_PROCESS_BUDGET ? XTC_HOST_BUDGET :
            status == XTC_PROCESS_OUT_OF_MEMORY ? XTC_HOST_OUT_OF_MEMORY :
            status == XTC_PROCESS_MEMORY_LIMIT ? XTC_HOST_MEMORY_LIMIT :
            status == XTC_PROCESS_OVERFLOW ? XTC_HOST_OVERFLOW :
            status == XTC_PROCESS_TIMEOUT ? XTC_HOST_TIMEOUT :
            status == XTC_PROCESS_UNRESOLVED ? XTC_HOST_UNRESOLVED :
            status == XTC_PROCESS_UNSUPPORTED ? XTC_HOST_UNSUPPORTED :
            status == XTC_PROCESS_INVALID ? XTC_HOST_INVALID : XTC_HOST_IO;
        return local_fail(owner, mapped, XTC_HOST_PROCESS, status);
    }
    return true;
}
static bool local_process_prepared(XtcHostToolchain *owner, const XrToolchainProcess *process,
    XrProcessResult *output) {
    if (!local_process_status(owner,xtc_process_run(process,NULL,NULL,output))) return false;
    if (output->stdout_bytes.truncated || output->stderr_bytes.truncated)
        return local_fail(owner, XTC_HOST_BUDGET, XTC_HOST_PROCESS, XTC_PROCESS_BUDGET);
    if (output->exit_code) {
        owner->diagnostic.child_exit = output->exit_code;
        return local_fail(owner, XTC_HOST_CHILD_FAILED, XTC_HOST_PROCESS, output->exit_code);
    }
    return true;
}
static bool local_process(XtcHostToolchain *owner, const XrProcessSpec *spec, XrProcessResult *output) {
    XrToolchainProcess *process=NULL;
    bool okay=local_process_status(owner,xtc_process_prepare(owner->resources,spec,&process));
    if (okay) okay=local_process_prepared(owner,process,output);
    xtc_process_free(process); return okay;
}
#endif
#ifdef XR_OS_WINDOWS
#include "xtc_host_toolchain_windows.inc.c"
#elif defined(XTC_HOST_POSIX)
#include "xtc_host_toolchain_posix.inc.c"
#endif

XR_FUNC XtcHostStatus xtc_host_toolchain_open(XrCompileResources *resources,
    const XtcHostToolchainRequest *request, XtcHostToolchain **output, XtcHostDiagnostic *diagnostic) {
    XtcHostDiagnostic initial = {XTC_HOST_INVALID, XTC_HOST_REQUEST, XTC_HOST_SELF, 0, 0, 0};
    if (!resources || !request || !output || *output || !request->timeout_ms ||
        !request->output_limit || request->output_limit == SIZE_MAX || (unsigned)request->crt > XTC_HOST_CRT_DYNAMIC_DEBUG) {
        if (diagnostic) *diagnostic = initial;
        return initial.status;
    }
#if !(defined(XR_OS_WINDOWS) && defined(XR_ARCH_X86_64)) && !defined(XTC_HOST_POSIX)
    initial.status = XTC_HOST_UNSUPPORTED;
    if (diagnostic) *diagnostic = initial;
    return initial.status;
#else
    XtcHostToolchain *owner = NULL;
    XrCompileResourceStatus allocation = xr_compile_resources_calloc(resources, 1, sizeof(*owner), (void **)&owner);
    if (allocation != XR_COMPILE_RESOURCE_OK) {
        initial.status = local_resource_status(allocation); initial.domain = XTC_HOST_RESOURCE; initial.code = allocation;
        if (diagnostic) *diagnostic = initial;
        return initial.status;
    }
    owner->resources = resources; owner->policy = xr_compile_io_policy(resources);
    owner->crt = request->crt;
    owner->diagnostic.stage = XTC_HOST_WORKSPACE;
    bool okay = local_workspace(owner, request->workspace_parent);
    if (okay) { owner->diagnostic.stage = XTC_HOST_DISCOVERY; okay = local_discover(owner, request); }
    XtcHostStatus status = owner->diagnostic.status;
    if (diagnostic) *diagnostic = owner->diagnostic;
    if (okay) *output = owner;
    else xtc_host_toolchain_free(owner);
    return status;
#endif
}
XR_FUNC const XtcHostToolchainView *xtc_host_toolchain_view(const XtcHostToolchain *owner) {
    return owner ? &owner->view : NULL;
}
XR_FUNC void xtc_host_toolchain_free(XtcHostToolchain *owner) {
    if (!owner) return;
    for (uint32_t i = 0; i < owner->count; ++i) xr_compile_resources_free(owner->strings[i]);
    xr_compile_resources_free(owner);
}
