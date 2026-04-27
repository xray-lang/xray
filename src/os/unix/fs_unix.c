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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>  // rename
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    if (stat(path, &st) != 0) {
        out->kind = XR_FS_NONE;
        out->size = 0;
        out->mtime_ns = 0;
        return -1;
    }
    out->kind = kind_from_mode(st.st_mode);
    out->size = (uint64_t) st.st_size;
#if defined(__APPLE__)
    out->mtime_ns = (int64_t) st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#elif defined(__linux__)
    out->mtime_ns = (int64_t) st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#else
    out->mtime_ns = (int64_t) st.st_mtime * 1000000000LL;
#endif
    return 0;
}

bool xr_fs_exists(const char *path) {
    if (path == NULL) {
        return false;
    }
    struct stat st;
    return stat(path, &st) == 0;
}

bool xr_fs_is_file(const char *path) {
    if (path == NULL) {
        return false;
    }
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool xr_fs_is_dir(const char *path) {
    if (path == NULL) {
        return false;
    }
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
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
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
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
