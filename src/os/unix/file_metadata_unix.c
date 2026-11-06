/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_metadata_unix.c - Complete stat metadata with an independent lstat fact
 */
#include "../os_file_metadata.h"
#include "../file_metadata_path.inc.h"
#include <sys/stat.h>
#include <unistd.h>

XR_FUNC XrOsIoStatus xr_os_io_file_metadata_close(XrOsFileMetadataOwner *owner) {
    if (!owner || owner->handle) return XR_OS_IO_BAD_ARGUMENT;
    return XR_OS_IO_OK;
}
static bool metadata_errno_refused(int error) {
    return error == ENOENT || error == ENOTDIR || error == EACCES || error == EPERM ||
        error == ELOOP || error == ENAMETOOLONG;
}
static XrOsIoStatus metadata_stat_fields(const struct stat *target, const struct stat *leaf,
    XrOsFileMetadata *output) {
    _Static_assert(sizeof(target->st_size) <= sizeof(int64_t), "stat size fits the signed output representation");
    _Static_assert(sizeof(target->st_mtime) <= sizeof(int64_t), "stat seconds fit the signed output representation");
    uintmax_t uid = (uintmax_t)target->st_uid, gid = (uintmax_t)target->st_gid;
    if (target->st_size < 0 || uid > INT64_MAX || gid > INT64_MAX)
        return XR_OS_IO_OVERFLOW;
    *output = (XrOsFileMetadata){true, (int64_t)target->st_size, (int64_t)(target->st_mode & 0777),
        (int64_t)target->st_mtime, (int64_t)target->st_atime, (int64_t)target->st_ctime,
        {true, (int64_t)uid}, {true, (int64_t)gid},
        S_ISREG(target->st_mode), S_ISDIR(target->st_mode), S_ISLNK(leaf->st_mode)};
    return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xr_os_io_file_metadata(const XrOsIoPolicy *policy,
    const char *path, size_t length, bool (*current)(void *context),
    XrOsFileMetadataOwner *owner, XrOsFileMetadata *output) {
    if (!io_policy_valid(policy) || !owner || owner->handle || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    char *terminated = NULL;
    XrOsFileMetadata result = {0};
    io_status(&io, metadata_path_owned(policy, path, length, current, &terminated));
    if (io.status == XR_OS_IO_OK) {
        struct stat target, leaf;
        if (stat(terminated, &target) != 0 || lstat(terminated, &leaf) != 0) {
            int error = errno;
            if (!metadata_errno_refused(error))
                io_status(&io, error == EOVERFLOW ? XR_OS_IO_OVERFLOW : io_errno_status(error));
        } else io_status(&io, metadata_stat_fields(&target, &leaf, &result));
    }
    io_free(&io, terminated);
    if (io.status == XR_OS_IO_OK && current && !current(policy->context)) io.status = XR_OS_IO_CANCELLED;
    if (io.status == XR_OS_IO_OK) *output = result;
    return io.status;
}
