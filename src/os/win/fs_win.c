/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fs_win.c - Windows implementation of os_fs.h.
 *
 * Uses ANSI APIs (FILE_ATTRIBUTE_*, CreateDirectoryA, ...) to keep
 * the calling-side surface a plain `const char *` path. This is
 * sufficient because xray's source paths come from the build, the
 * package registry, or the command line, all of which are UTF-8 /
 * ASCII. Windows-CP code-page rounding for non-ASCII paths is a
 * known limitation that will be addressed when we add explicit
 * UTF-8 marshalling.
 */

#include "../os_fs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Convert FILETIME (100-ns ticks since 1601-01-01) to ns since unix epoch.
static int64_t filetime_to_unix_ns(FILETIME ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // 116444736000000000 = ticks between 1601-01-01 and 1970-01-01
    if (u.QuadPart < 116444736000000000ULL) {
        return 0;
    }
    uint64_t unix_100ns = u.QuadPart - 116444736000000000ULL;
    return (int64_t) (unix_100ns * 100ULL);
}

int xr_fs_stat(const char *path, XrFsStat *out) {
    if (path == NULL || out == NULL) {
        return -1;
    }
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) {
        out->kind = XR_FS_NONE;
        out->size = 0;
        out->mtime_ns = 0;
        return -1;
    }
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        out->kind = XR_FS_DIR;
        out->size = 0;
    } else if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        out->kind = XR_FS_OTHER;
        out->size = 0;
    } else {
        out->kind = XR_FS_FILE;
        ULARGE_INTEGER sz;
        sz.LowPart = d.nFileSizeLow;
        sz.HighPart = d.nFileSizeHigh;
        out->size = sz.QuadPart;
    }
    out->mtime_ns = filetime_to_unix_ns(d.ftLastWriteTime);
    return 0;
}

bool xr_fs_exists(const char *path) {
    if (path == NULL) {
        return false;
    }
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool xr_fs_is_file(const char *path) {
    if (path == NULL) {
        return false;
    }
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool xr_fs_is_dir(const char *path) {
    if (path == NULL) {
        return false;
    }
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int xr_fs_mkdir(const char *path, unsigned int mode) {
    (void) mode;  // Windows ACL is not modeled here.
    if (path == NULL) {
        return -1;
    }
    if (CreateDirectoryA(path, NULL)) {
        return 0;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS && xr_fs_is_dir(path)) {
        return 0;
    }
    return -1;
}

int xr_fs_remove(const char *path) {
    if (path == NULL) {
        return -1;
    }
    return DeleteFileA(path) ? 0 : -1;
}

int xr_fs_rename(const char *old_path, const char *new_path) {
    if (old_path == NULL || new_path == NULL) {
        return -1;
    }
    // MOVEFILE_REPLACE_EXISTING matches POSIX rename() semantics.
    return MoveFileExA(old_path, new_path, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

char *xr_fs_realpath(const char *path, char *out, size_t out_size) {
    if (path == NULL || out == NULL || out_size == 0) {
        return NULL;
    }
    DWORD n = GetFullPathNameA(path, (DWORD) out_size, out, NULL);
    if (n == 0 || n >= out_size) {
        return NULL;
    }
    return out;
}

char *xr_fs_getcwd(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return NULL;
    }
    DWORD n = GetCurrentDirectoryA((DWORD) out_size, out);
    if (n == 0 || n >= out_size) {
        return NULL;
    }
    return out;
}

int xr_fs_chdir(const char *path) {
    if (path == NULL) {
        return -1;
    }
    return SetCurrentDirectoryA(path) ? 0 : -1;
}
