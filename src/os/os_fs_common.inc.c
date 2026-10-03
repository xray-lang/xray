/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_fs_common.inc.c - Policy queries sharing the platform stat and path owners
 */
static XrOsIoStatus fs_predicate(const XrOsIoPolicy *policy, const char *path,
    XrFsKind kind, bool *output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
    XrFsStat stat;
    XrOsIoStatus status = xr_os_io_stat(policy, path, &stat);
    if (status == XR_OS_IO_OK) *output = kind == XR_FS_NONE || stat.kind == kind;
    return status;
}
XR_FUNC XrOsIoStatus xr_os_io_exists(const XrOsIoPolicy *policy, const char *path, bool *output) {
    return fs_predicate(policy, path, XR_FS_NONE, output);
}
XR_FUNC XrOsIoStatus xr_os_io_is_file(const XrOsIoPolicy *policy, const char *path, bool *output) {
    return fs_predicate(policy, path, XR_FS_FILE, output);
}
XR_FUNC XrOsIoStatus xr_os_io_is_dir(const XrOsIoPolicy *policy, const char *path, bool *output) {
    return fs_predicate(policy, path, XR_FS_DIR, output);
}
XR_FUNC XrOsIoStatus xr_os_io_realpath(const XrOsIoPolicy *policy, const char *path,
    char *output, size_t capacity) {
    if (!io_policy_valid(policy) || !path || !output || !capacity) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; char *resolved = NULL; size_t length = 0;
    io_status(&io, xr_realpath_owned(policy, path, &resolved));
    if (io.status == XR_OS_IO_OK && io_length(&io, resolved, &length)) {
        if (length >= capacity) io_status(&io, XR_OS_IO_BUDGET);
        else io_copy(&io, output, resolved, length + 1);
    }
    io_free(&io, resolved);
    return io.status;
}
