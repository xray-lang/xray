/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_mutation_unix.c - Native POSIX path mutation results with typed faults
 */
#include "../os_file_mutation.h"
#include "../file_metadata_path.inc.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static XrOsIoStatus file_mutation_error(int error) {
    if (error == EACCES || error == EPERM || error == EROFS || error == ENOENT || error == ENOTDIR ||
        error == EISDIR || error == ENOTEMPTY || error == EEXIST || error == EXDEV || error == EBUSY ||
        error == ELOOP || error == ENAMETOOLONG || error == EINVAL || error == ENOSPC || error == EMLINK)
        return XR_OS_IO_OK;
#if defined(EDQUOT)
    if (error == EDQUOT) return XR_OS_IO_OK;
#endif
    return error == EOVERFLOW ? XR_OS_IO_OVERFLOW : io_errno_status(error);
}
static bool file_mutation_current(const XrOsIoPolicy *policy, bool (*current)(void *), XrIoContext *io) {
    return !current || current(policy->context) || io_status(io, XR_OS_IO_CANCELLED);
}
typedef enum FileMutationKind { FILE_MUTATION_MKDIR, FILE_MUTATION_RMDIR, FILE_MUTATION_REMOVE,
    FILE_MUTATION_UTIME } FileMutationKind;
static XrOsIoStatus file_mutation_one(const XrOsIoPolicy *policy, XrOsPathSpan path,
    bool (*current)(void *), FileMutationKind kind, bool *output) {
    if (!io_policy_valid(policy) || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    char *terminated = NULL; bool changed = false;
    io_status(&io, metadata_path_owned(policy, path.text, path.length, current, &terminated));
    if (io.status == XR_OS_IO_OK && file_mutation_current(policy, current, &io)) {
        int result;
        switch (kind) {
        case FILE_MUTATION_MKDIR: result = mkdir(terminated, 0755); break;
        case FILE_MUTATION_RMDIR: result = rmdir(terminated); break;
        case FILE_MUTATION_REMOVE: result = remove(terminated); break;
        case FILE_MUTATION_UTIME: result = utimensat(AT_FDCWD, terminated, NULL, 0); break;
        default: result = -1; errno = EINVAL; break;
        }
        changed = result == 0;
        if (!changed) {
            int error = errno;
            if (kind == FILE_MUTATION_MKDIR && error == EEXIST) {
                struct stat existing;
                if (stat(terminated, &existing) == 0) changed = S_ISDIR(existing.st_mode);
                else io_status(&io, file_mutation_error(errno));
            } else io_status(&io, file_mutation_error(error));
        }
    }
    io_free(&io, terminated);
    if (io.status == XR_OS_IO_OK) *output = changed;
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_path_mkdir(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *), bool *output) {
    return file_mutation_one(policy, path, current, FILE_MUTATION_MKDIR, output);
}
XR_FUNC XrOsIoStatus xr_os_io_path_rmdir(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *), bool *output) {
    return file_mutation_one(policy, path, current, FILE_MUTATION_RMDIR, output);
}
XR_FUNC XrOsIoStatus xr_os_io_path_remove(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *), XrOsFileMetadataOwner *owner, bool *output) {
    if (!owner || owner->handle) return XR_OS_IO_BAD_ARGUMENT;
    return file_mutation_one(policy, path, current, FILE_MUTATION_REMOVE, output);
}
XR_FUNC XrOsIoStatus xr_os_io_path_utime_now(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *), XrOsFileMetadataOwner *owner, bool *output) {
    if (!owner || owner->handle) return XR_OS_IO_BAD_ARGUMENT;
    return file_mutation_one(policy, path, current, FILE_MUTATION_UTIME, output);
}
XR_FUNC XrOsIoStatus xr_os_io_path_rename(const XrOsIoPolicy *policy,
    XrOsPathSpan old_path, XrOsPathSpan new_path, bool (*current)(void *), bool *output) {
    if (!io_policy_valid(policy) || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    char *from = NULL, *to = NULL; bool changed = false;
    io_status(&io, metadata_path_owned(policy, old_path.text, old_path.length, current, &from));
    if (io.status == XR_OS_IO_OK)
        io_status(&io, metadata_path_owned(policy, new_path.text, new_path.length, current, &to));
    if (io.status == XR_OS_IO_OK && file_mutation_current(policy, current, &io)) {
        changed = rename(from, to) == 0;
        if (!changed) io_status(&io, file_mutation_error(errno));
    }
    io_free(&io, to); io_free(&io, from);
    if (io.status == XR_OS_IO_OK) *output = changed;
    return io.status;
}
