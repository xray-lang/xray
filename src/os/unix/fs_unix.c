/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fs_unix.c - POSIX implementation of os_fs.h.
 */

#include "../os_fs.h"
#include "../../base/xfileio.h"
#include "../../base/xio_policy.inc.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>

static bool path_ready(XrIoContext *io, const char *path) {
    size_t length = 0;
    return io_length(io, path, &length);
}
static void close_file(XrIoContext *io, int fd) {
    if (fd >= 0 && close(fd)) io_status(io, io_errno_status(errno));
}
XR_FUNC XrOsIoStatus xr_os_io_lock_exclusive(const XrOsIoPolicy *policy, const char *path, XrFsExclusiveLock *out) {
    if (!io_policy_valid(policy) || !path || !out) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; int fd = -1; struct stat stat;
    if (path_ready(&io, path) && io_work(&io, 1)) {
        fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) io_status(&io, io_errno_status(errno));
    }
    if (io_work(&io, 1)) {
        if (fstat(fd, &stat)) io_status(&io, io_errno_status(errno));
        else if (!S_ISREG(stat.st_mode)) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
    }
    if (io_work(&io, 1) && flock(fd, LOCK_EX)) io_status(&io, io_errno_status(errno));
    if (io.status == XR_OS_IO_OK) out->handle = (intptr_t)fd + 1;
    else close_file(&io, fd);
    return io.status;
}
XR_FUNC int xr_fs_unlock_exclusive(XrFsExclusiveLock *lock) {
    if (!lock || !lock->handle) return -1;
    int fd = (int)(lock->handle - 1);
    bool unlocked = flock(fd, LOCK_UN) == 0, closed = close(fd) == 0;
    lock->handle = 0; return unlocked && closed ? 0 : -1;
}
static XrFsStat stat_value(const struct stat *stat) {
    XrFsStat result = {S_ISREG(stat->st_mode) ? XR_FS_FILE : S_ISDIR(stat->st_mode) ? XR_FS_DIR : XR_FS_OTHER,
        (uint64_t)stat->st_size, 0};
#if defined(XR_OS_MACOS)
    result.mtime_ns = (int64_t)stat->st_mtimespec.tv_sec * 1000000000LL + stat->st_mtimespec.tv_nsec;
#elif defined(XR_OS_LINUX)
    result.mtime_ns = (int64_t)stat->st_mtim.tv_sec * 1000000000LL + stat->st_mtim.tv_nsec;
#else
    result.mtime_ns = (int64_t)stat->st_mtime * 1000000000LL;
#endif
    return result;
}
XR_FUNC XrOsIoStatus xr_os_io_stat(const XrOsIoPolicy *policy, const char *path, XrFsStat *out) {
    if (!io_policy_valid(policy) || !path || !out) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; struct stat stat;
    if (path_ready(&io, path) && io_work(&io, 1)) {
        if (lstat(path, &stat)) io_status(&io, io_errno_status(errno));
        else *out = stat_value(&stat);
    }
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_mkdir(const XrOsIoPolicy *policy, const char *path, unsigned int mode) {
    if (!io_policy_valid(policy) || !path) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    if (path_ready(&io, path) && io_work(&io, 1) && mkdir(path, (mode_t)mode)) {
        int error = errno;
        if (error == EEXIST && io_work(&io, 1)) {
            struct stat st;
            if (stat(path, &st)) io_status(&io, io_errno_status(errno));
            else if (!S_ISDIR(st.st_mode)) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
        } else io_status(&io, io_errno_status(error));
    }
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_remove(const XrOsIoPolicy *policy, const char *path) {
    if (!io_policy_valid(policy) || !path) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    if (path_ready(&io, path) && io_work(&io, 1) && unlink(path)) io_status(&io, io_errno_status(errno));
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_rename(const XrOsIoPolicy *policy, const char *from, const char *to) {
    if (!io_policy_valid(policy) || !from || !to) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    if (path_ready(&io, from) && path_ready(&io, to) && io_work(&io, 1) && rename(from, to))
        io_status(&io, io_errno_status(errno));
    return io.status;
}
static int open_regular(XrIoContext *io, const char *path, int flags, uint64_t *size) {
    int fd = -1;
    if (path_ready(io, path) && io_work(io, 1)) {
        fd = open(path, flags | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) io_status(io, io_errno_status(errno));
    }
    if (io_work(io, 1)) {
        struct stat st;
        if (fstat(fd, &st)) io_status(io, io_errno_status(errno));
        else if (!S_ISREG(st.st_mode) || st.st_size < 0) io_status(io, XR_OS_IO_BAD_ARGUMENT);
        else if (size) *size = (uint64_t)st.st_size;
    }
    return fd;
}
static void file_transfer(XrIoContext *io, int fd, void *bytes, size_t size, bool writing) {
    size_t offset = 0;
    while (io->status == XR_OS_IO_OK && offset < size) {
        size_t chunk = size - offset > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : size - offset;
        if (!io_work(io, 1 + chunk)) break;
        ssize_t count = writing ? write(fd, (const char *)bytes + offset, chunk) : read(fd, (char *)bytes + offset, chunk);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) io_status(io, io_errno_status(errno));
        else if (!count) io_status(io, XR_OS_IO_IO);
        else offset += (size_t)count;
    }
}
XR_FUNC XrOsIoStatus xr_os_io_read_regular_file(const XrOsIoPolicy *policy, const char *path,
    size_t maximum, uint8_t **output, size_t *output_size) {
    if (!io_policy_valid(policy) || !path || !output || !output_size) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; uint64_t width = 0; uint8_t *bytes = NULL;
    int fd = open_regular(&io, path, O_RDONLY, &width);
    if (io.status == XR_OS_IO_OK) {
        if (width > maximum || width > SIZE_MAX) io_status(&io, XR_OS_IO_BUDGET);
        else bytes = io_alloc(&io, width ? (size_t)width : 1);
    }
    if (bytes) file_transfer(&io, fd, bytes, (size_t)width, false);
    while (io_work(&io, 2)) {
        uint8_t extra; ssize_t count = read(fd, &extra, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) io_status(&io, io_errno_status(errno));
        else if (count) io_status(&io, XR_OS_IO_IO);
        break;
    }
    close_file(&io, fd);
    if (io.status == XR_OS_IO_OK) { *output = bytes; *output_size = (size_t)width; }
    else io_free(&io, bytes);
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_write_new_file_sync(const XrOsIoPolicy *policy, const char *path,
    const uint8_t *data, size_t size) {
    if (!io_policy_valid(policy) || !path || (!data && size)) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; int fd = -1;
    if (path_ready(&io, path) && io_work(&io, 1)) {
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) io_status(&io, io_errno_status(errno));
    }
    file_transfer(&io, fd, (void *)data, size, true);
    if (io_work(&io, 1) && fsync(fd)) io_status(&io, io_errno_status(errno));
    close_file(&io, fd);
    if (fd >= 0 && io.status != XR_OS_IO_OK) (void)unlink(path);
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_publish_noreplace(const XrOsIoPolicy *policy, const char *temp_path, const char *final_path) {
    if (!io_policy_valid(policy) || !temp_path || !final_path) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; XrFsStat stat;
    io_status(&io, xr_os_io_stat(policy, temp_path, &stat));
    if (io.status == XR_OS_IO_OK && stat.kind != XR_FS_FILE) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
    if (path_ready(&io, final_path) && io_work(&io, 1) && link(temp_path, final_path)) io_status(&io, io_errno_status(errno));
    if (io_work(&io, 1) && unlink(temp_path)) io_status(&io, io_errno_status(errno));
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_sync_directory(const XrOsIoPolicy *policy, const char *path) {
    if (!io_policy_valid(policy) || !path) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; int fd = -1;
    if (path_ready(&io, path) && io_work(&io, 1)) {
        fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) io_status(&io, io_errno_status(errno));
    }
    if (io_work(&io, 1) && fsync(fd)) io_status(&io, io_errno_status(errno));
    close_file(&io, fd); return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_touch(const XrOsIoPolicy *policy, const char *path) {
    if (!io_policy_valid(policy) || !path) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; int fd = open_regular(&io, path, O_RDONLY, NULL);
    if (io_work(&io, 1) && futimens(fd, NULL)) io_status(&io, io_errno_status(errno));
    close_file(&io, fd); return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_getcwd(const XrOsIoPolicy *policy, char *out, size_t capacity) {
    if (!io_policy_valid(policy) || !out || !capacity) return XR_OS_IO_BAD_ARGUMENT;
    if (capacity == SIZE_MAX) return XR_OS_IO_BUDGET;
    XrIoContext io = {policy, XR_OS_IO_OK}; char *scratch = io_alloc(&io, capacity);
    if (scratch && io_work(&io, 1 + capacity) && !getcwd(scratch, capacity)) io_status(&io, io_errno_status(errno));
    size_t length = 0;
    if (io.status == XR_OS_IO_OK && io_length(&io, scratch, &length)) io_copy(&io, out, scratch, length + 1);
    io_free(&io, scratch); return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_chdir(const XrOsIoPolicy *policy, const char *path) {
    if (!io_policy_valid(policy) || !path) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    if (path_ready(&io, path) && io_work(&io, 1) && chdir(path)) io_status(&io, io_errno_status(errno));
    return io.status;
}
#include "../os_fs_common.inc.c"
