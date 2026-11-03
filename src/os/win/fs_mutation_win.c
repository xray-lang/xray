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

XR_FUNC XrOsIoStatus xr_os_io_chdir(const XrOsIoPolicy *policy, const char *path,
    bool (*current)(void *context)) {
    if (!io_policy_valid(policy) || !path) return XR_OS_IO_BAD_ARGUMENT;
    if (current && !current(policy->context)) return XR_OS_IO_CANCELLED;
    XrIoContext io = {policy, XR_OS_IO_OK}; wchar_t *wide = NULL;
    io_status(&io, xr_win_utf8_path_owned(policy, path, &wide));
    if (io.status == XR_OS_IO_OK && current && !current(policy->context))
        io_status(&io, XR_OS_IO_CANCELLED);
    if (((&io)->status == XR_OS_IO_OK) && !SetCurrentDirectoryW(wide)) io_status(&io, io_windows_status(GetLastError()));
    io_free(&io, wide); return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_chmod(const XrOsIoPolicy *policy, const char *path, int64_t mode_value,
    bool (*current)(void *context)) {
    int mode = 0;
    if (!io_policy_valid(policy) || !path || !xr_io_core_chmod_mode(mode_value, &mode))
        return XR_OS_IO_BAD_ARGUMENT;
    if (current && !current(policy->context)) return XR_OS_IO_CANCELLED;
    XrIoContext io = {policy, XR_OS_IO_OK}; wchar_t *wide = NULL;
    io_status(&io, xr_win_utf8_path_owned(policy, path, &wide));
    if (io.status == XR_OS_IO_OK && current && !current(policy->context))
        io_status(&io, XR_OS_IO_CANCELLED);
    if (io.status == XR_OS_IO_OK && _wchmod(wide, mode) != 0)
        io_status(&io, io_errno_status(errno));
    io_free(&io, wide); return io.status;
}
