/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_host_toolchain_posix.inc.c - One C driver and its observed facts
 *
 * KEY CONCEPT:
 *   macOS asks xcrun for its selected SDK and compiler. Linux resolves its
 *   requested C driver. Owned paths and a private command environment survive
 *   the synchronous discovery operation.
 */

/* POSIX discovery uses TMPDIR, then /tmp, for the private workspace parent. */
static bool local_workspace(XtcHostToolchain *owner, const char *explicit_parent) {
    if (explicit_parent) return local_path(owner, explicit_parent, true, &owner->view.workspace_parent);
    char *candidate = NULL;
    XrOsIoStatus status = xr_os_io_environment_get(&owner->policy, "TMPDIR", &candidate);
    if (status != XR_OS_IO_NOT_FOUND && !local_io(owner, status)) return false;
    bool okay = local_path(owner, candidate && candidate[0] ? candidate : "/tmp", true,
        &owner->view.workspace_parent);
    xr_compile_resources_free(candidate); return okay;
}
#ifdef XR_OS_MACOS
/* One output line without its newline; other control bytes are rejected. */
static bool local_line(XtcHostToolchain *owner, const XrProcessByteBuffer *bytes, char **output) {
    if (!bytes->data || bytes->truncated) return local_fail(owner, XTC_HOST_UNRESOLVED, XTC_HOST_SELF, 0);
    size_t end = bytes->length;
    while (end && (bytes->data[end - 1] == '\n' || bytes->data[end - 1] == '\r')) --end;
    if (!end) return local_fail(owner, XTC_HOST_UNRESOLVED, XTC_HOST_SELF, 0);
    for (size_t i = 0; i < end; ++i) {

        if (bytes->data[i] < 32) return local_fail(owner, XTC_HOST_UNSUPPORTED, XTC_HOST_SELF, 0);
    }
    *output = local_copy(owner, (const char *)bytes->data, end); return *output != NULL;
}
static bool local_xcrun(XtcHostToolchain *owner, const XtcHostToolchainRequest *request,
    const char *option, const char *argument, char **output) {
    XrProcessSpec spec; XrProcessResult result = {0};
    xtc_process_spec_init(&spec, "/usr/bin/xcrun", request->timeout_ms);
    spec.environment_source = XTC_PROCESS_ENV_SNAPSHOT; spec.cwd = "/";
    spec.output_limit = request->output_limit;
    spec.argv[1] = option; spec.argv[2] = argument;
    bool okay = local_process(owner, &spec, &result) && local_line(owner, &result.stdout_bytes, output);
    xtc_process_result_free(&result); return okay;
}
#endif
/* Resolve a candidate that may not exist. Absent paths, and paths that do not
 * name the requested kind, yield NULL without a failure; links are followed. */
static bool local_existing(XtcHostToolchain *owner, const char *path, bool directory, char **output) {
    char *resolved = NULL;
    XrOsIoStatus status = xr_realpath_owned(&owner->policy, path, &resolved);
    if (status == XR_OS_IO_NOT_FOUND) return true;
    if (!local_io(owner, status)) return false;
    XrFsStat stat = {0};
    status = xr_os_io_stat(&owner->policy, resolved, &stat);
    if (status == XR_OS_IO_OK && stat.kind == (directory ? XR_FS_DIR : XR_FS_FILE)) { *output = resolved; return true; }
    xr_compile_resources_free(resolved);
    return status == XR_OS_IO_OK || status == XR_OS_IO_NOT_FOUND || local_io(owner, status);
}
/* A name without a slash is searched in the absolute PATH entries only. */
static bool local_search(XtcHostToolchain *owner, const char *name) {
    char *search = NULL, *candidate = NULL, *resolved = NULL;
    XrOsIoStatus status = xr_os_io_environment_get(&owner->policy, "PATH", &search);
    if (status == XR_OS_IO_NOT_FOUND) return local_fail(owner, XTC_HOST_UNRESOLVED, XTC_HOST_FILESYSTEM, status);
    bool okay = local_io(owner, status), found = false;
    for (size_t at = 0; okay && !found && search[at];) {
        size_t end = at;
        while (search[end] && search[end] != ':') ++end;
        char saved = search[end]; search[end] = 0;
        if (search[at] == '/') {
            okay = local_io(owner, xr_path_join_owned(&owner->policy, search + at, name, &candidate)) &&
                local_existing(owner, candidate, false, &resolved);
            if (okay && resolved) { found = true; okay = local_path(owner, resolved, false, &owner->view.compiler); }
            xr_compile_resources_free(candidate); candidate = NULL;
            xr_compile_resources_free(resolved); resolved = NULL;
        }
        search[end] = saved; at = saved ? end + 1 : end;
    }
    xr_compile_resources_free(search);
    if (okay && !found) okay = local_fail(owner, XTC_HOST_UNRESOLVED, XTC_HOST_FILESYSTEM, XR_OS_IO_NOT_FOUND);
    return okay;
}
static bool local_compiler(XtcHostToolchain *owner, const char *name) {
    for (const char *c = name; *c; ++c)
        if (*c == '/') return local_path(owner, name, false, &owner->view.compiler);
    return name[0] ? local_search(owner, name) : local_fail(owner, XTC_HOST_INVALID, XTC_HOST_SELF, 0);
}
/* Commands see the driver's own directory, then the system tool directories. */
static bool local_command_path(XtcHostToolchain *owner) {
    char *directory = NULL; size_t length = 0;
    if (!local_io(owner, xr_path_dirname_owned(&owner->policy, owner->view.compiler, &directory))) return false;
    bool okay = local_length(owner, directory, &length);
    static const char suffix[] = ":/usr/bin:/bin";
    char *path = okay ? local_allocate(owner, length + sizeof(suffix)) : NULL;
    if (path) { memcpy(path, directory, length); memcpy(path + length, suffix, sizeof(suffix)); }
    xr_compile_resources_free(directory);
    return path && local_keep(owner, path, &owner->view.posix.path);
}
static bool local_discover(XtcHostToolchain *owner, const XtcHostToolchainRequest *request) {
    const char *selected = request->compiler;
    bool okay = true;
#ifdef XR_OS_MACOS
    char *sdk = NULL, *clang = NULL;
    okay = local_xcrun(owner, request, "--show-sdk-path", NULL, &sdk) &&
        local_path(owner, sdk, true, &owner->view.posix.sysroot) &&
        (selected || local_xcrun(owner, request, "--find", "clang", &clang));
    if (okay && !selected) selected = clang;
    owner->view.posix.arch = XTC_HOST_ARCH;
#else
    if (!selected) selected = "/usr/bin/cc";
#endif
    if (okay) owner->diagnostic.stage = XTC_HOST_PATHS;
    if (okay) okay = local_compiler(owner, selected) && local_command_path(owner);
#ifdef XR_OS_MACOS
    xr_compile_resources_free(sdk); xr_compile_resources_free(clang);
#endif
    if (okay) { owner->view.linker = owner->view.compiler; owner->diagnostic.stage = XTC_HOST_PATHS; }
    return okay;
}
