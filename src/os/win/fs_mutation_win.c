/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fs_mutation_win.c - Owned wide-path filesystem mutations.
 */

#include "../os_fs.h"
#include "../../base/xwindows_utf8.h"
#include "../../shared/xr_io_core.h"
#include <io.h>
#include <sys/stat.h>

static bool mutation_errno_refused(int error) {
    return error == EACCES || error == EPERM || error == EINVAL || error == ENOENT ||
        error == ENOTDIR || error == ENAMETOOLONG || error == ERANGE;
}
XR_FUNC XrOsIoStatus xr_os_io_chdir(const XrOsIoPolicy *policy, const char *path,
    bool (*current)(void *context), bool *output) {
    if (!io_policy_valid(policy) || !path || !output) return XR_OS_IO_BAD_ARGUMENT;
    if (current && !current(policy->context)) return XR_OS_IO_CANCELLED;
    XrIoContext io = {policy, XR_OS_IO_OK}; wchar_t *wide = NULL;
    bool refused = false, changed = false;
    io_status(&io, xr_win_utf8_path_owned_classified(policy, path, &wide, &refused));
    if (io.status == XR_OS_IO_OK || refused) {
        if (current && !current(policy->context)) io.status = XR_OS_IO_CANCELLED;
        else if (refused) io.status = XR_OS_IO_OK;
        else {
            SetLastError(ERROR_SUCCESS);
            changed = SetCurrentDirectoryW(wide) != 0;
            if (!changed) {
                DWORD error = GetLastError();
                if (!xr_win_path_error_refused(error)) io_status(&io, io_windows_status(error));
            }
        }
    }
    io_free(&io, wide);
    if (io.status == XR_OS_IO_OK) *output = changed;
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_chmod(const XrOsIoPolicy *policy, const char *path, int64_t mode_value,
    bool (*current)(void *context), bool *output) {
    int mode = 0;
    if (!io_policy_valid(policy) || !path || !output || !xr_io_core_chmod_mode(mode_value, &mode))
        return XR_OS_IO_BAD_ARGUMENT;
    if (current && !current(policy->context)) return XR_OS_IO_CANCELLED;
    XrIoContext io = {policy, XR_OS_IO_OK}; wchar_t *wide = NULL;
    bool refused = false, changed = false;
    io_status(&io, xr_win_utf8_path_owned_classified(policy, path, &wide, &refused));
    if (io.status == XR_OS_IO_OK || refused) {
        if (current && !current(policy->context)) io.status = XR_OS_IO_CANCELLED;
        else if (refused) io.status = XR_OS_IO_OK;
        else {
            errno = 0;
            changed = _wchmod(wide, mode) == 0;
            if (!changed && !mutation_errno_refused(errno)) io_status(&io, io_errno_status(errno));
        }
    }
    io_free(&io, wide);
    if (io.status == XR_OS_IO_OK) *output = changed;
    return io.status;
}
