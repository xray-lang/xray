/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fs_mutation_unix.c - POSIX filesystem mutations with pre-write cancellation.
 */

#include "../os_fs.h"
#include "../../base/xio_policy.inc.h"
#include "../../shared/xr_io_core.h"
#include <sys/stat.h>
#include <unistd.h>

static bool mutation_path_ready(XrIoContext *io, const char *path) {
    size_t length = 0;
    return io_length(io, path, &length);
}
static bool mutation_errno_refused(int error) {
    return error == EACCES || error == EPERM || error == EINVAL || error == ENOENT ||
        error == ENOTDIR || error == ENAMETOOLONG || error == ERANGE || error == ELOOP ||
        error == EROFS || error == EIO || error == EBUSY;
}
XR_FUNC XrOsIoStatus xr_os_io_chdir(const XrOsIoPolicy *policy, const char *path,
    bool (*current)(void *context), bool *output) {
    if (!io_policy_valid(policy) || !path || !output) return XR_OS_IO_BAD_ARGUMENT;
    if (current && !current(policy->context)) return XR_OS_IO_CANCELLED;
    XrIoContext io = {policy, XR_OS_IO_OK};
    if (!mutation_path_ready(&io, path)) return io.status;
    if (current && !current(policy->context)) return XR_OS_IO_CANCELLED;
    errno = 0;
    bool changed = chdir(path) == 0;
    if (!changed && !mutation_errno_refused(errno)) io_status(&io, io_errno_status(errno));
    if (io.status == XR_OS_IO_OK) *output = changed;
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_chmod(const XrOsIoPolicy *policy, const char *path, int64_t mode_value,
    bool (*current)(void *context), bool *output) {
    int mode = 0;
    if (!io_policy_valid(policy) || !path || !output || !xr_io_core_chmod_mode(mode_value, &mode))
        return XR_OS_IO_BAD_ARGUMENT;
    if (current && !current(policy->context)) return XR_OS_IO_CANCELLED;
    XrIoContext io = {policy, XR_OS_IO_OK};
    if (!mutation_path_ready(&io, path)) return io.status;
    if (current && !current(policy->context)) return XR_OS_IO_CANCELLED;
    errno = 0;
    bool changed = chmod(path, (mode_t)mode) == 0;
    if (!changed && !mutation_errno_refused(errno)) io_status(&io, io_errno_status(errno));
    if (io.status == XR_OS_IO_OK) *output = changed;
    return io.status;
}
