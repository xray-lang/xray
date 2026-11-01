/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_read_unix.c - Descriptor-bound physical-root and bounded file reads
 *
 * KEY CONCEPT:
 *   Resolve links explicitly, authorize the final physical parent by inode,
 *   and read only through the verified regular-file descriptor.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* Strict POSIX mode hides O_NOFOLLOW on Darwin; other hosts ignore this macro. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "../os_file_read.h"
#include "../../base/xio_policy.inc.h"
#include "../../shared/xr_utf8_core.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    FILE_PATH_BYTES = 32768,
    FILE_ANCESTOR_LIMIT = 256,
    FILE_LINK_LIMIT = 40
};
#define DIRECTORY_FLAGS (O_RDONLY | O_DIRECTORY | O_CLOEXEC)

static XrFileReadStatus file_io_status(XrOsIoStatus status) {
    switch (status) {
        case XR_OS_IO_OK:
            return XR_FILE_READ_OK;
        case XR_OS_IO_OVERFLOW:
            return XR_FILE_READ_OVERFLOW;
        case XR_OS_IO_MEMORY_LIMIT:
            return XR_FILE_READ_MEMORY_LIMIT;
        case XR_OS_IO_OUT_OF_MEMORY:
            return XR_FILE_READ_OUT_OF_MEMORY;
        case XR_OS_IO_BAD_ARGUMENT:
            return XR_FILE_READ_FORBIDDEN;
        default:
            return XR_FILE_READ_IO;
    }
}

static XrFileReadStatus file_errno_status(int error) {
    return error == ELOOP ? XR_FILE_READ_FORBIDDEN : file_io_status(io_errno_status(error));
}

static bool path_length(const char *path, size_t *length) {
    for (size_t n = 0; n < FILE_PATH_BYTES; ++n) {
        if (!path[n]) {
            *length = n;
            return true;
        }
    }
    return false;
}

/* Logical locators have the same portable segment restrictions on every host. */
static bool logical_path_valid(const char *path, size_t length) {
    if (xr_utf8_core_scan_strict((const uint8_t *) path, length).error != XR_UTF8_OK)
        return false;
    size_t segment = 0;
    for (size_t n = 0; n <= length; ++n) {
        unsigned char c = (unsigned char) path[n];
        if (!c || c == '/') {
            if (n == segment || path[n - 1] == '.' || path[n - 1] == ' ')
                return false;
            segment = n + 1;
        } else if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '?' || c == '*') {
            return false;
        }
    }
    return true;
}

static bool same_file(const struct stat *first, const struct stat *second) {
    return first->st_dev == second->st_dev && first->st_ino == second->st_ino;
}

/* Directory identities also handle root aliases, case folding and mount points. */
static XrFileReadStatus physical_ancestor(int directory, const struct stat *root) {
    int current = openat(directory, ".", DIRECTORY_FLAGS);
    if (current < 0)
        return file_errno_status(errno);
    XrFileReadStatus status = XR_FILE_READ_LIMIT;
    for (unsigned depth = 0; depth < FILE_ANCESTOR_LIMIT; ++depth) {
        struct stat information, parent_info;
        if (fstat(current, &information)) {
            status = file_errno_status(errno);
            break;
        }
        if (same_file(&information, root)) {
            status = XR_FILE_READ_OK;
            break;
        }
        int parent = openat(current, "..", DIRECTORY_FLAGS);
        if (parent < 0) {
            status = file_errno_status(errno);
            break;
        }
        if (fstat(parent, &parent_info)) {
            status = file_errno_status(errno);
            close(parent);
            break;
        }
        bool top = same_file(&information, &parent_info);
        close(current);
        current = parent;
        if (top) {
            status = XR_FILE_READ_FORBIDDEN;
            break;
        }
    }
    close(current);
    return status;
}

static XrFileReadStatus read_bytes(XrIoContext *io, int file, size_t limit, XrFileBytes *output) {
    struct stat before, after;
    if (fstat(file, &before))
        return file_errno_status(errno);
    if (!S_ISREG(before.st_mode))
        return XR_FILE_READ_FORBIDDEN;
    if (before.st_size < 0)
        return XR_FILE_READ_IO;
    if ((uintmax_t) before.st_size > limit || (uintmax_t) before.st_size >= SIZE_MAX)
        return XR_FILE_READ_LIMIT;
    size_t length = (size_t) before.st_size, offset = 0;
    char *data = io_alloc(io, length + 1);
    if (!data)
        return file_io_status(io->status);
    while (offset < length) {
        size_t count = length - offset;
        if (count > 65536)
            count = 65536;
        ssize_t got = read(file, data + offset, count);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            goto fail;
        offset += (size_t) got;
    }
    char extra;
    ssize_t got;
    do {
        got = read(file, &extra, 1);
    } while (got < 0 && errno == EINTR);
    if (got != 0 || fstat(file, &after) || after.st_size != before.st_size)
        goto fail;
    data[length] = 0;
    *output = (XrFileBytes) {data, length};
    return XR_FILE_READ_OK;
fail:
    io_free(io, data);
    return XR_FILE_READ_IO;
}

/* Keep the remaining suffix when following either an absolute or relative link. */
static XrFileReadStatus expand_link(XrIoContext *io, int directory, const char *name,
                                    const char *remaining, char **pending) {
    char *target = io_alloc(io, FILE_PATH_BYTES);
    if (!target)
        return file_io_status(io->status);
    ssize_t got = readlinkat(directory, name, target, FILE_PATH_BYTES);
    XrFileReadStatus status = XR_FILE_READ_OK;
    if (got <= 0)
        status = got < 0 ? file_errno_status(errno) : XR_FILE_READ_FORBIDDEN;
    else {
        size_t length = (size_t) got, suffix = remaining ? strlen(remaining) : 0;
        size_t separator = remaining ? 1 : 0;
        if (length >= FILE_PATH_BYTES || suffix + separator >= FILE_PATH_BYTES - length)
            status = XR_FILE_READ_LIMIT;
        else {
            if (remaining)
                target[length++] = '/';
            if (suffix)
                memcpy(target + length, remaining, suffix);
            target[length + suffix] = 0;
            io_free(io, *pending);
            *pending = target;
            return XR_FILE_READ_OK;
        }
    }
    io_free(io, target);
    return status;
}

static XrFileReadStatus resolve_and_read(XrIoContext *io, int root, const struct stat *root_info,
                                         char **pending, size_t limit, XrFileBytes *output) {
    int directory = openat(root, ".", DIRECTORY_FLAGS);
    if (directory < 0)
        return file_errno_status(errno);
    char *cursor = *pending;
    unsigned links = 0;
    XrFileReadStatus status = XR_FILE_READ_FORBIDDEN;
    for (;;) {
        while (*cursor == '/')
            ++cursor;
        if (!*cursor)
            break;
        char *slash = strchr(cursor, '/');
        if (slash)
            *slash = 0;
        const char *remaining = slash ? slash + 1 : NULL;
        struct stat information;
        if (fstatat(directory, cursor, &information, AT_SYMLINK_NOFOLLOW)) {
            int error = errno;
            status = file_errno_status(error);
            if (error == ENOENT) {
                status = physical_ancestor(directory, root_info);
                if (status == XR_FILE_READ_OK)
                    status = XR_FILE_READ_MISSING;
            }
            break;
        }
        if (S_ISLNK(information.st_mode)) {
            if (++links > FILE_LINK_LIMIT) {
                status = XR_FILE_READ_LIMIT;
                break;
            }
            status = expand_link(io, directory, cursor, remaining, pending);
            if (status != XR_FILE_READ_OK)
                break;
            cursor = *pending;
            if (*cursor == '/') {
                int absolute = open("/", DIRECTORY_FLAGS);
                if (absolute < 0) {
                    status = file_errno_status(errno);
                    break;
                }
                close(directory);
                directory = absolute;
            }
            /* A link that resolves to a directory still cannot become file input. */
            status = XR_FILE_READ_FORBIDDEN;
            continue;
        }
        if (remaining) {
            if (!S_ISDIR(information.st_mode)) {
                status = XR_FILE_READ_FORBIDDEN;
                break;
            }
            int next = openat(directory, cursor, DIRECTORY_FLAGS | O_NOFOLLOW);
            if (next < 0) {
                status = file_errno_status(errno);
                break;
            }
            close(directory);
            directory = next;
            cursor = slash + 1;
            continue;
        }
        if (!S_ISREG(information.st_mode)) {
            status = XR_FILE_READ_FORBIDDEN;
            break;
        }
        status = physical_ancestor(directory, root_info);
        if (status != XR_FILE_READ_OK)
            break;
        /* Nonblocking mode also prevents a concurrent FIFO replacement from hanging. */
        int file = openat(directory, cursor, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (file < 0) {
            status = file_errno_status(errno);
            break;
        }
        struct stat opened;
        if (fstat(file, &opened))
            status = file_errno_status(errno);
        else if (!same_file(&information, &opened))
            status = XR_FILE_READ_IO;
        else
            status = read_bytes(io, file, limit, output);
        close(file);
        break;
    }
    close(directory);
    return status;
}

XR_FUNC XrFileReadStatus xr_os_io_read_under_root(const XrOsIoPolicy *policy, const char *root,
                                                  const char *logical_path, size_t limit,
                                                  XrFileBytes *output) {
    if (!io_policy_valid(policy) || !root || !logical_path || !output)
        return XR_FILE_READ_BAD_ARGUMENT;
    size_t root_length = 0, length = 0;
    if (!path_length(logical_path, &length))
        return XR_FILE_READ_LIMIT;
    if (!logical_path_valid(logical_path, length))
        return XR_FILE_READ_FORBIDDEN;
    if (!path_length(root, &root_length))
        return XR_FILE_READ_LIMIT;
    if (root[0] != '/' ||
        xr_utf8_core_scan_strict((const uint8_t *) root, root_length).error != XR_UTF8_OK)
        return XR_FILE_READ_FORBIDDEN;
    XrIoContext io = {policy, XR_OS_IO_OK};
    int directory = open(root, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (directory < 0)
        return file_errno_status(errno);
    struct stat root_info;
    XrFileReadStatus status = XR_FILE_READ_FORBIDDEN;
    char *pending = NULL;
    if (fstat(directory, &root_info))
        status = file_errno_status(errno);
    else if (S_ISDIR(root_info.st_mode)) {
        pending = io_alloc(&io, length + 1);
        if (pending) {
            memcpy(pending, logical_path, length + 1);
            status = resolve_and_read(&io, directory, &root_info, &pending, limit, output);
        }
    }
    close(directory);
    io_free(&io, pending);
    return io.status == XR_OS_IO_OK ? status : file_io_status(io.status);
}
