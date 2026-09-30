/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fs_win.c - Windows implementation of os_fs.h.
 *
 * Filesystem operations accept strict UTF-8 locators and use wide Win32 calls.
 * File-kind and publication rules are independent of path encoding.
 */

#include "../os_fs.h"
#include "../../base/xwindows_utf8.h"

#include "../../base/xmalloc.h"
#include "../../base/xfileio.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>


static HANDLE open_utf8_file(const char *path, DWORD access, DWORD share, DWORD creation, DWORD flags) {
    XrWinPathStatus status;
    wchar_t *wide = xr_win_utf8_path(path, &status);
    if (!wide) return INVALID_HANDLE_VALUE;
    HANDLE handle = CreateFileW(wide, access, share, NULL, creation, flags, NULL);
    xr_free(wide);
    return handle;
}
static bool move_utf8_file(const char *from, const char *to, DWORD flags, DWORD *error) {
    XrWinPathStatus status;
    if (error) *error = ERROR_INVALID_NAME;
    wchar_t *wide_from = xr_win_utf8_path(from, &status);
    wchar_t *wide_to = wide_from ? xr_win_utf8_path(to, &status) : NULL;
    bool ok = wide_from && wide_to && MoveFileExW(wide_from, wide_to, flags);
    if (wide_from && wide_to && error) *error = ok ? ERROR_SUCCESS : GetLastError();
    xr_free(wide_to); xr_free(wide_from);
    return ok;
}

int xr_fs_lock_exclusive(const char *path, XrFsExclusiveLock *out) {
    if (!path || !out)
        return -1;
    out->handle = 0;
    XrFsStat stat;
    if (xr_fs_stat(path, &stat) == 0 && stat.kind != XR_FS_FILE)
        return -1;
    HANDLE handle = open_utf8_file(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT);
    if (handle == INVALID_HANDLE_VALUE)
        return -1;
    BY_HANDLE_FILE_INFORMATION info;
    OVERLAPPED overlapped = {0};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped)) {
        CloseHandle(handle);
        return -1;
    }
    out->handle = (intptr_t) handle;
    return 0;
}

int xr_fs_unlock_exclusive(XrFsExclusiveLock *lock) {
    if (!lock || !lock->handle)
        return -1;
    HANDLE handle = (HANDLE) lock->handle;
    OVERLAPPED overlapped = {0};
    bool unlocked = UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped) != 0;
    bool closed = CloseHandle(handle) != 0;
    lock->handle = 0;
    return unlocked && closed ? 0 : -1;
}

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
    XrWinPathStatus converted;
    wchar_t *wide = xr_win_utf8_path(path, &converted);
    WIN32_FILE_ATTRIBUTE_DATA d = {0};
    bool found = wide && GetFileAttributesExW(wide, GetFileExInfoStandard, &d);
    xr_free(wide);
    if (!found) {
        out->kind = XR_FS_NONE;
        out->size = 0;
        out->mtime_ns = 0;
        return -1;
    }
    if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        out->kind = XR_FS_OTHER;
        out->size = 0;
    } else if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        out->kind = XR_FS_DIR;
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
    (void) mode;  // Windows ACL is not modeled here.
    if (path == NULL) {
        return -1;
    }
    XrWinPathStatus status;
    wchar_t *wide = xr_win_utf8_path(path, &status);
    if (!wide) return -1;
    bool ok = CreateDirectoryW(wide, NULL) != 0;
    if (!ok && GetLastError() == ERROR_ALREADY_EXISTS) {
        DWORD attributes = GetFileAttributesW(wide);
        ok = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    }
    xr_free(wide);
    return ok ? 0 : -1;
}

int xr_fs_remove(const char *path) {
    if (path == NULL) {
        return -1;
    }
    XrWinPathStatus status;
    wchar_t *wide = xr_win_utf8_path(path, &status);
    if (!wide) return -1;
    bool ok = DeleteFileW(wide) != 0;
    xr_free(wide);
    return ok ? 0 : -1;
}

int xr_fs_rename(const char *old_path, const char *new_path) {
    if (old_path == NULL || new_path == NULL) {
        return -1;
    }
    // MOVEFILE_REPLACE_EXISTING matches POSIX rename() semantics.
    return move_utf8_file(old_path, new_path, MOVEFILE_REPLACE_EXISTING, NULL) ? 0 : -1;
}

static bool win_regular_handle(HANDLE handle, uint64_t *size) {
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return false;
    if (size) {
        ULARGE_INTEGER value;
        value.LowPart = info.nFileSizeLow;
        value.HighPart = info.nFileSizeHigh;
        *size = value.QuadPart;
    }
    return true;
}

int xr_fs_read_regular_file(const char *path, size_t max_size, uint8_t **out_bytes,
                            size_t *out_size) {
    if (!path || !out_bytes || !out_size)
        return -1;
    *out_bytes = NULL;
    *out_size = 0;
    HANDLE handle = open_utf8_file(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                                       FILE_SHARE_DELETE, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                    FILE_FLAG_SEQUENTIAL_SCAN);
    if (handle == INVALID_HANDLE_VALUE)
        return -1;
    uint64_t width = 0;
    bool ok = win_regular_handle(handle, &width) && width <= (uint64_t) max_size &&
              width <= (uint64_t) SIZE_MAX;
    size_t size = ok ? (size_t) width : 0;
    uint8_t *bytes = ok ? (uint8_t *) xr_malloc(size ? size : 1) : NULL;
    if (ok && !bytes)
        ok = false;
    size_t offset = 0;
    while (ok && offset < size) {
        DWORD chunk = size - offset > MAXDWORD ? MAXDWORD : (DWORD) (size - offset);
        DWORD count = 0;
        if (!ReadFile(handle, bytes + offset, chunk, &count, NULL) || count == 0) {
            ok = false;
            break;
        }
        offset += count;
    }
    uint8_t extra;
    DWORD extra_count = 0;
    if (ok && (!ReadFile(handle, &extra, 1, &extra_count, NULL) || extra_count != 0))
        ok = false;
    if (!CloseHandle(handle))
        ok = false;
    if (!ok) {
        xr_free(bytes);
        return -1;
    }
    *out_bytes = bytes;
    *out_size = size;
    return 0;
}

int xr_fs_write_new_file_sync(const char *path, const uint8_t *data, size_t size) {
    if (!path || (!data && size != 0))
        return -1;
    HANDLE handle = open_utf8_file(path, GENERIC_WRITE, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT);
    if (handle == INVALID_HANDLE_VALUE)
        return -1;
    size_t offset = 0;
    bool ok = true;
    while (offset < size) {
        DWORD chunk = size - offset > MAXDWORD ? MAXDWORD : (DWORD) (size - offset);
        DWORD count = 0;
        if (!WriteFile(handle, data + offset, chunk, &count, NULL) || count == 0) {
            ok = false;
            break;
        }
        offset += count;
    }
    if (ok)
        ok = FlushFileBuffers(handle) != 0;
    if (!CloseHandle(handle))
        ok = false;
    if (!ok) {
        (void) xr_fs_remove(path);
        return -1;
    }
    return 0;
}

XrFsPublishResult xr_fs_publish_noreplace(const char *temp_path, const char *final_path) {
    if (!temp_path || !final_path)
        return XR_FS_PUBLISH_ERROR;
    HANDLE handle = open_utf8_file(temp_path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT);
    if (handle == INVALID_HANDLE_VALUE)
        return XR_FS_PUBLISH_ERROR;
    bool regular = win_regular_handle(handle, NULL);
    CloseHandle(handle);
    if (!regular)
        return XR_FS_PUBLISH_ERROR;
    DWORD error;
    if (move_utf8_file(temp_path, final_path, MOVEFILE_WRITE_THROUGH, &error))
        return XR_FS_PUBLISH_OK;
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
        return XR_FS_PUBLISH_EXISTS;
    return XR_FS_PUBLISH_ERROR;
}

XrFsSyncResult xr_fs_sync_directory(const char *path) {
    if (!path || !path[0])
        return XR_FS_SYNC_ERROR;
    return XR_FS_SYNC_UNSUPPORTED;
}

int xr_fs_touch(const char *path) {
    if (!path)
        return -1;
    HANDLE handle = open_utf8_file(path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT);
    if (handle == INVALID_HANDLE_VALUE)
        return -1;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    bool ok = win_regular_handle(handle, NULL) && SetFileTime(handle, NULL, NULL, &now) != 0;
    if (!CloseHandle(handle))
        ok = false;
    return ok ? 0 : -1;
}

char *xr_fs_realpath(const char *path, char *out, size_t out_size) {
    if (path == NULL || out == NULL || out_size == 0) {
        return NULL;
    }
    char *resolved = xr_realpath(path);
    if (!resolved) return NULL;
    size_t size = strlen(resolved) + 1;
    if (size <= out_size) memcpy(out, resolved, size);
    xr_free(resolved);
    return size <= out_size ? out : NULL;
}

char *xr_fs_getcwd(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return NULL;
    }
    DWORD units = GetCurrentDirectoryW(0, NULL);
    if (!units || units > 32768) return NULL;
    wchar_t *wide = xr_malloc((size_t)units * sizeof(wchar_t));
    if (!wide) return NULL;
    DWORD length = GetCurrentDirectoryW(units, wide);
    bool ok = length && length < units && xr_win_utf16_to_utf8(wide, length, out, out_size);
    xr_free(wide);
    return ok ? out : NULL;
}

int xr_fs_chdir(const char *path) {
    if (path == NULL) {
        return -1;
    }
    XrWinPathStatus status;
    wchar_t *wide = xr_win_utf8_path(path, &status);
    if (!wide) return -1;
    bool ok = SetCurrentDirectoryW(wide) != 0;
    xr_free(wide);
    return ok ? 0 : -1;
}
