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
#include "../../base/xfileio.h"

static HANDLE open_file(XrIoContext *io, const char *path, DWORD access, DWORD share,
    DWORD creation, DWORD flags, wchar_t **held_path) {
    wchar_t *wide = NULL;
    io_status(io, xr_win_utf8_path_owned(io->policy, path, &wide));
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (io_work(io, 1)) {
        handle = CreateFileW(wide, access, share, NULL, creation, flags, NULL);
        if (handle == INVALID_HANDLE_VALUE) io_status(io, io_windows_status(GetLastError()));
    }
    if (held_path) *held_path = wide;
    else io_free(io, wide);
    return handle;
}
static bool regular_handle(XrIoContext *io, HANDLE handle, uint64_t *size) {
    BY_HANDLE_FILE_INFORMATION info;
    if (!io_work(io, 1)) return false;
    if (GetFileType(handle) != FILE_TYPE_DISK) return io_status(io, XR_OS_IO_BAD_ARGUMENT);
    if (!io_work(io, 1)) return false;
    if (!GetFileInformationByHandle(handle, &info)) return io_status(io, io_windows_status(GetLastError()));
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        return io_status(io, XR_OS_IO_BAD_ARGUMENT);
    if (size) *size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    return true;
}
static void close_file(XrIoContext *io, HANDLE handle) {
    if (handle != INVALID_HANDLE_VALUE && !CloseHandle(handle)) io_status(io, io_windows_status(GetLastError()));
}
XR_FUNC XrOsIoStatus xr_os_io_lock_exclusive(const XrOsIoPolicy *policy, const char *path, XrFsExclusiveLock *out) {
    if (!io_policy_valid(policy) || !path || !out) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    HANDLE handle = open_file(&io, path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    OVERLAPPED overlapped = {0};
    if (io.status == XR_OS_IO_OK && regular_handle(&io, handle, NULL) && io_work(&io, 1) &&
        !LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped))
        io_status(&io, io_windows_status(GetLastError()));
    if (io.status == XR_OS_IO_OK) out->handle = (intptr_t)handle;
    else close_file(&io, handle);
    return io.status;
}
XR_FUNC int xr_fs_unlock_exclusive(XrFsExclusiveLock *lock) {
    if (!lock || !lock->handle) return -1;
    HANDLE handle = (HANDLE)lock->handle; OVERLAPPED overlapped = {0};
    bool unlocked = UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped) != 0;
    bool closed = CloseHandle(handle) != 0; lock->handle = 0;
    return unlocked && closed ? 0 : -1;
}
static int64_t filetime_to_unix_ns(FILETIME value) {
    uint64_t ticks = ((uint64_t)value.dwHighDateTime << 32) | value.dwLowDateTime;
    return ticks < 116444736000000000ULL ? 0 : (int64_t)((ticks - 116444736000000000ULL) * 100ULL);
}
XR_FUNC XrOsIoStatus xr_os_io_stat(const XrOsIoPolicy *policy, const char *path, XrFsStat *out) {
    if (!io_policy_valid(policy) || !path || !out) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; wchar_t *wide = NULL;
    io_status(&io, xr_win_utf8_path_owned(policy, path, &wide));
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!io_work(&io, 1)) { io_free(&io, wide); return io.status; }
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data))
        io_status(&io, io_windows_status(GetLastError()));
    io_free(&io, wide);
    if (io.status == XR_OS_IO_OK) {
        XrFsKind kind = data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ? XR_FS_OTHER :
            data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? XR_FS_DIR : XR_FS_FILE;
        *out = (XrFsStat){kind, kind == XR_FS_FILE ? ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow : 0,
            filetime_to_unix_ns(data.ftLastWriteTime)};
    }
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_mkdir(const XrOsIoPolicy *policy, const char *path, unsigned int mode) {
    (void)mode;
    if (!io_policy_valid(policy) || !path) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; wchar_t *wide = NULL;
    io_status(&io, xr_win_utf8_path_owned(policy, path, &wide));
    if (io_work(&io, 1) && !CreateDirectoryW(wide, NULL)) {
        DWORD error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS && io_work(&io, 1)) {
            DWORD attributes = GetFileAttributesW(wide);
            if (attributes == INVALID_FILE_ATTRIBUTES) io_status(&io, io_windows_status(GetLastError()));
            else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
        } else io_status(&io, io_windows_status(error));
    }
    io_free(&io, wide); return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_remove(const XrOsIoPolicy *policy, const char *path) {
    if (!io_policy_valid(policy) || !path) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; wchar_t *wide = NULL;
    io_status(&io, xr_win_utf8_path_owned(policy, path, &wide));
    if (io_work(&io, 1) && !DeleteFileW(wide)) io_status(&io, io_windows_status(GetLastError()));
    io_free(&io, wide); return io.status;
}
static XrOsIoStatus move_file(const XrOsIoPolicy *policy, const char *from, const char *to, DWORD flags) {
    if (!io_policy_valid(policy) || !from || !to) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; wchar_t *wide_from = NULL, *wide_to = NULL;
    io_status(&io, xr_win_utf8_path_owned(policy, from, &wide_from));
    if (io.status == XR_OS_IO_OK) io_status(&io, xr_win_utf8_path_owned(policy, to, &wide_to));
    if (io_work(&io, 1) && !MoveFileExW(wide_from, wide_to, flags)) io_status(&io, io_windows_status(GetLastError()));
    io_free(&io, wide_to); io_free(&io, wide_from); return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_rename(const XrOsIoPolicy *policy, const char *from, const char *to) {
    return move_file(policy, from, to, MOVEFILE_REPLACE_EXISTING);
}
XR_FUNC XrOsIoStatus xr_os_io_read_regular_file(const XrOsIoPolicy *policy, const char *path,
    size_t maximum, uint8_t **output, size_t *output_size) {
    if (!io_policy_valid(policy) || !path || !output || !output_size) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    HANDLE handle = open_file(&io, path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    uint64_t width = 0; uint8_t *bytes = NULL;
    if (io.status == XR_OS_IO_OK && regular_handle(&io, handle, &width)) {
        if (width > maximum || width > SIZE_MAX) io_status(&io, XR_OS_IO_BUDGET);
        else bytes = io_alloc(&io, width ? (size_t)width : 1);
    }
    size_t offset = 0;
    while (io.status == XR_OS_IO_OK && offset < (size_t)width) {
        DWORD chunk = width - offset > MAXDWORD ? MAXDWORD : (DWORD)(width - offset), count = 0;
        if (!io_work(&io, 1 + (uint64_t)chunk)) break;
        if (!ReadFile(handle, bytes + offset, chunk, &count, NULL)) io_status(&io, io_windows_status(GetLastError()));
        else if (!count) io_status(&io, XR_OS_IO_IO);
        else offset += count;
    }
    uint8_t extra; DWORD count = 0;
    if (io_work(&io, 2)) {
        if (!ReadFile(handle, &extra, 1, &count, NULL)) io_status(&io, io_windows_status(GetLastError()));
        else if (count) io_status(&io, XR_OS_IO_IO);
    }
    close_file(&io, handle);
    if (io.status == XR_OS_IO_OK) { *output = bytes; *output_size = (size_t)width; }
    else io_free(&io, bytes);
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_write_new_file_sync(const XrOsIoPolicy *policy, const char *path,
    const uint8_t *data, size_t size) {
    if (!io_policy_valid(policy) || !path || (!data && size)) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; wchar_t *wide = NULL;
    HANDLE handle = open_file(&io, path, GENERIC_WRITE, 0, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, &wide);
    size_t offset = 0;
    while (io.status == XR_OS_IO_OK && offset < size) {
        DWORD chunk = size - offset > MAXDWORD ? MAXDWORD : (DWORD)(size - offset), count = 0;
        if (!io_work(&io, 1 + (uint64_t)chunk)) break;
        if (!WriteFile(handle, data + offset, chunk, &count, NULL)) io_status(&io, io_windows_status(GetLastError()));
        else if (!count) io_status(&io, XR_OS_IO_IO);
        else offset += count;
    }
    if (io_work(&io, 1) && !FlushFileBuffers(handle)) io_status(&io, io_windows_status(GetLastError()));
    close_file(&io, handle);
    if (handle != INVALID_HANDLE_VALUE && io.status != XR_OS_IO_OK) (void)DeleteFileW(wide);
    io_free(&io, wide); return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_publish_noreplace(const XrOsIoPolicy *policy, const char *temp_path, const char *final_path) {
    if (!io_policy_valid(policy) || !temp_path || !final_path) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    HANDLE handle = open_file(&io, temp_path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (io.status == XR_OS_IO_OK) regular_handle(&io, handle, NULL);
    close_file(&io, handle);
    return io.status == XR_OS_IO_OK ? move_file(policy, temp_path, final_path, MOVEFILE_WRITE_THROUGH) : io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_sync_directory(const XrOsIoPolicy *policy, const char *path) {
    if (!io_policy_valid(policy) || !path || !*path) return XR_OS_IO_BAD_ARGUMENT;
    return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_touch(const XrOsIoPolicy *policy, const char *path) {
    if (!io_policy_valid(policy) || !path) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    HANDLE handle = open_file(&io, path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    FILETIME now;
    if (io.status == XR_OS_IO_OK && regular_handle(&io, handle, NULL) && io_work(&io, 1)) {
        GetSystemTimeAsFileTime(&now);
        if (io_work(&io, 1) && !SetFileTime(handle, NULL, NULL, &now)) io_status(&io, io_windows_status(GetLastError()));
    }
    close_file(&io, handle); return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_getcwd(const XrOsIoPolicy *policy, char *out, size_t capacity) {
    if (!io_policy_valid(policy) || !out || !capacity) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; DWORD units = 0; wchar_t *wide = NULL; char *text = NULL;
    if (io_work(&io, 1)) {
        units = GetCurrentDirectoryW(0, NULL);
        if (!units || units > 32768) io_status(&io, units ? XR_OS_IO_BUDGET : io_windows_status(GetLastError()));
    }
    if (io.status == XR_OS_IO_OK) wide = io_alloc(&io, (size_t)units * sizeof(wchar_t));
    if (wide && io_work(&io, 1 + (size_t)units * sizeof(wchar_t))) {
        DWORD length = GetCurrentDirectoryW(units, wide);
        if (!length || length >= units) io_status(&io, length ? XR_OS_IO_BUDGET : io_windows_status(GetLastError()));
    }
    if (io.status == XR_OS_IO_OK) io_status(&io, xr_win_utf16_text_owned(policy, wide, &text));
    size_t length = 0;
    if (io.status == XR_OS_IO_OK && io_length(&io, text, &length)) {
        if (length >= capacity) io_status(&io, XR_OS_IO_BUDGET);
        else io_copy(&io, out, text, length + 1);
    }
    io_free(&io, text); io_free(&io, wide); return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_chdir(const XrOsIoPolicy *policy, const char *path) {
    if (!io_policy_valid(policy) || !path) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; wchar_t *wide = NULL;
    io_status(&io, xr_win_utf8_path_owned(policy, path, &wide));
    if (io_work(&io, 1) && !SetCurrentDirectoryW(wide)) io_status(&io, io_windows_status(GetLastError()));
    io_free(&io, wide); return io.status;
}
#include "../os_fs_common.inc.c"
