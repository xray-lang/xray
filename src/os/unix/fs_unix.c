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

#include "../../base/xmalloc.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <stdint.h>
#include <stdio.h>  // rename
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int xr_fs_lock_exclusive(const char *path, XrFsExclusiveLock *out) {
    if (!path || !out)
        return -1;
    out->handle = 0;
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || flock(fd, LOCK_EX) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    out->handle = (intptr_t) fd + 1;
    return 0;
}

int xr_fs_unlock_exclusive(XrFsExclusiveLock *lock) {
    if (!lock || !lock->handle)
        return -1;
    int fd = (int) (lock->handle - 1);
    bool ok = flock(fd, LOCK_UN) == 0 && close(fd) == 0;
    lock->handle = 0;
    return ok ? 0 : -1;
}

static XrFsKind kind_from_mode(mode_t m) {
    if (S_ISREG(m))
        return XR_FS_FILE;
    if (S_ISDIR(m))
        return XR_FS_DIR;
    return XR_FS_OTHER;
}

int xr_fs_stat(const char *path, XrFsStat *out) {
    if (path == NULL || out == NULL) {
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) != 0) {
        out->kind = XR_FS_NONE;
        out->size = 0;
        out->mtime_ns = 0;
        return -1;
    }
    out->kind = kind_from_mode(st.st_mode);
    out->size = (uint64_t) st.st_size;
#if defined(XR_OS_MACOS)
    out->mtime_ns = (int64_t) st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#elif defined(XR_OS_LINUX)
    out->mtime_ns = (int64_t) st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#else
    out->mtime_ns = (int64_t) st.st_mtime * 1000000000LL;
#endif
    return 0;
}

bool xr_fs_exists(const char *path) {
    XrFsStat st;
    return xr_fs_stat(path, &st) == 0;
}

bool xr_fs_is_file(const char *path) {
    XrFsStat st;
    return xr_fs_stat(path, &st) == 0 && st.kind == XR_FS_FILE;
}

bool xr_fs_is_dir(const char *path) {
    XrFsStat st;
    return xr_fs_stat(path, &st) == 0 && st.kind == XR_FS_DIR;
}

int xr_fs_mkdir(const char *path, unsigned int mode) {
    if (path == NULL) {
        return -1;
    }
    if (mkdir(path, (mode_t) mode) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            return 0;
        }
    }
    return -1;
}

int xr_fs_remove(const char *path) {
    if (path == NULL) {
        return -1;
    }
    return unlink(path) == 0 ? 0 : -1;
}

int xr_fs_rename(const char *old_path, const char *new_path) {
    if (old_path == NULL || new_path == NULL) {
        return -1;
    }
    return rename(old_path, new_path) == 0 ? 0 : -1;
}

static int read_full(int fd, uint8_t *bytes, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = read(fd, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        offset += (size_t) count;
    }
    return 0;
}

int xr_fs_read_regular_file(const char *path, size_t max_size, uint8_t **out_bytes,
                            size_t *out_size) {
    if (!path || !out_bytes || !out_size)
        return -1;
    *out_bytes = NULL;
    *out_size = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    struct stat st;
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
             (uint64_t) st.st_size <= (uint64_t) max_size &&
             (uint64_t) st.st_size <= (uint64_t) SIZE_MAX;
    size_t size = ok ? (size_t) st.st_size : 0;
    uint8_t *bytes = ok ? (uint8_t *) xr_malloc(size ? size : 1) : NULL;
    if (ok && !bytes)
        ok = 0;
    if (ok && read_full(fd, bytes, size) != 0)
        ok = 0;
    uint8_t extra;
    ssize_t extra_count;
    do {
        extra_count = ok ? read(fd, &extra, 1) : 0;
    } while (extra_count < 0 && errno == EINTR);
    if (ok && extra_count != 0)
        ok = 0;
    if (close(fd) != 0)
        ok = 0;
    if (!ok) {
        xr_free(bytes);
        return -1;
    }
    *out_bytes = bytes;
    *out_size = size;
    return 0;
}

static int write_full(int fd, const uint8_t *bytes, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = write(fd, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        offset += (size_t) count;
    }
    return 0;
}

int xr_fs_write_new_file_sync(const char *path, const uint8_t *data, size_t size) {
    if (!path || (!data && size != 0))
        return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    int ok = write_full(fd, data, size) == 0 && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = 0;
    if (!ok) {
        (void) unlink(path);
        return -1;
    }
    return 0;
}

XrFsPublishResult xr_fs_publish_noreplace(const char *temp_path, const char *final_path) {
    if (!temp_path || !final_path)
        return XR_FS_PUBLISH_ERROR;
    struct stat st;
    if (lstat(temp_path, &st) != 0 || !S_ISREG(st.st_mode))
        return XR_FS_PUBLISH_ERROR;
    if (link(temp_path, final_path) != 0) {
        if (errno == EEXIST)
            return XR_FS_PUBLISH_EXISTS;
        return XR_FS_PUBLISH_ERROR;
    }
    if (unlink(temp_path) != 0)
        return XR_FS_PUBLISH_ERROR;
    return XR_FS_PUBLISH_OK;
}

XrFsSyncResult xr_fs_sync_directory(const char *path) {
    if (!path || !path[0])
        return XR_FS_SYNC_ERROR;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return XR_FS_SYNC_ERROR;
    int ok = fsync(fd) == 0;
    if (close(fd) != 0)
        ok = 0;
    return ok ? XR_FS_SYNC_OK : XR_FS_SYNC_ERROR;
}

int xr_fs_touch(const char *path) {
    if (!path)
        return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    struct stat st;
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && futimens(fd, NULL) == 0;
    if (close(fd) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

char *xr_fs_realpath(const char *path, char *out, size_t out_size) {
    if (path == NULL || out == NULL || out_size == 0) {
        return NULL;
    }
    char *resolved = realpath(path, NULL);
    if (resolved == NULL) {
        return NULL;
    }
    size_t n = strlen(resolved);
    if (n + 1 > out_size) {
        free(resolved);  // xr:allow-raw-alloc realpath() returns malloc'd buf
        return NULL;
    }
    memcpy(out, resolved, n + 1);
    free(resolved);  // xr:allow-raw-alloc
    return out;
}

char *xr_fs_getcwd(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return NULL;
    }
    return getcwd(out, out_size);
}

int xr_fs_chdir(const char *path) {
    if (path == NULL) {
        return -1;
    }
    return chdir(path) == 0 ? 0 : -1;
}
