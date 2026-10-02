/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdir_unix.c - POSIX implementation of xdir.h.
 *
 * Wraps DIR* / dirent. The iterator caches the directory path so
 * the is_dir fallback (when d_type is DT_UNKNOWN, e.g. some
 * network filesystems) can stat the full child path without the
 * caller having to plumb that path through.
 */

#include "../os_dir.h"
#include "../../base/xio_policy.inc.h"
#include <dirent.h>
#include <sys/stat.h>

struct XrDirIter {
    XrOsIoPolicy policy;
    DIR *directory;
    char *prefix;
    size_t prefix_length;
    XrDirEntry pending;
    XrOsIoStatus status;
    bool ended;
};
XR_FUNC XrOsIoStatus xr_os_io_dir_open(const XrOsIoPolicy *policy, const char *path, XrDirIter **output) {
    if (!io_policy_valid(policy) || !path || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; size_t length = 0;
    if (!io_length(&io, path, &length)) return io.status;
    if (!length) return XR_OS_IO_BAD_ARGUMENT;
    if (length > SIZE_MAX - 2) return XR_OS_IO_BUDGET;
    XrDirIter *iterator = io_alloc(&io, sizeof(*iterator));
    if (!iterator) return io.status;
    if (!io_clear(&io, iterator, sizeof(*iterator))) { io_free(&io, iterator); return io.status; }
    iterator->policy = *policy;
    iterator->prefix = io_alloc(&io, length + 2);
    if (iterator->prefix && io_copy(&io, iterator->prefix, path, length) && io_work(&io, 1)) {
        if (path[length - 1] != '/' && io_work(&io, 1)) iterator->prefix[length++] = '/';
        if (io_work(&io, 1)) iterator->prefix[length] = 0;
        iterator->prefix_length = length;
    }
    if (io_work(&io, 1)) {
        iterator->directory = opendir(path);
        if (!iterator->directory) io_status(&io, io_errno_status(errno));
    }
    if (io.status != XR_OS_IO_OK) xr_os_io_dir_close(iterator);
    else *output = iterator;
    return io.status;
}
static void classify_entry(XrIoContext *io, XrDirIter *iterator, const struct dirent *entry, size_t length) {
#ifdef DT_DIR
    if (entry->d_type == DT_DIR) { iterator->pending.is_dir = true; return; }
    if (entry->d_type != DT_UNKNOWN) return;
#endif
    if (length > SIZE_MAX - iterator->prefix_length - 1) { io_status(io, XR_OS_IO_BUDGET); return; }
    char *full = io_alloc(io, iterator->prefix_length + length + 1);
    if (full && io_copy(io, full, iterator->prefix, iterator->prefix_length) &&
        io_copy(io, full + iterator->prefix_length, entry->d_name, length + 1) && io_work(io, 1)) {
        struct stat info;
        if (stat(full, &info)) io_status(io, io_errno_status(errno));
        else iterator->pending.is_dir = S_ISDIR(info.st_mode);
    }
    io_free(io, full);
}
XR_FUNC XrOsIoStatus xr_os_io_dir_next(XrDirIter *iterator, XrDirEntry *output) {
    if (!iterator || !output) return XR_OS_IO_BAD_ARGUMENT;
    if (iterator->status != XR_OS_IO_OK) return iterator->status;
    if (iterator->ended) return XR_OS_IO_END;
    XrIoContext io = {&iterator->policy, XR_OS_IO_OK};
    while (io_work(&io, 1)) {
        errno = 0;
        struct dirent *entry = readdir(iterator->directory);
        if (!entry) {
            if (!errno) { iterator->ended = true; return XR_OS_IO_END; }
            io_status(&io, io_errno_status(errno)); break;
        }
        const char *name = entry->d_name;
        bool dot = false;
        if (io_work(&io, 1) && name[0] == '.' && io_work(&io, 1)) {
            if (!name[1]) dot = true;
            else if (name[1] == '.' && io_work(&io, 1) && !name[2]) dot = true;
        }
        if (dot) continue;
        size_t length = 0;
        if (io.status == XR_OS_IO_OK && io_length(&io, name, &length)) {
            if (length >= XR_DIR_ENTRY_NAME_MAX) io_status(&io, XR_OS_IO_BUDGET);
            else if (io_clear(&io, &iterator->pending, sizeof(iterator->pending)) &&
                io_copy(&io, iterator->pending.name, name, length + 1)) {
                classify_entry(&io, iterator, entry, length);
                io_copy(&io, output, &iterator->pending, sizeof(*output));
            }
        }
        break;
    }
    iterator->status = io.status; return io.status;
}
XR_FUNC void xr_os_io_dir_close(XrDirIter *iterator) {
    if (!iterator) return;
    if (iterator->directory) closedir(iterator->directory);
    XrOsIoPolicy policy = iterator->policy;
    if (iterator->prefix) policy.free(policy.context, iterator->prefix);
    policy.free(policy.context, iterator);
}
