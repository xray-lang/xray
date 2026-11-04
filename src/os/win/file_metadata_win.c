/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_metadata_win.c - Wide following metadata and name-surrogate detection
 */
#include "../os_file_metadata.h"
#include "../file_metadata_path.inc.h"
#include "../../base/xwindows_utf8.h"
#include <sys/stat.h>

XR_FUNC XrOsIoStatus xr_os_io_file_metadata_close(XrOsFileMetadataOwner *owner) {
    if (!owner) return XR_OS_IO_BAD_ARGUMENT;
    if (!owner->handle) return XR_OS_IO_OK;
    if (!CloseHandle((HANDLE)owner->handle)) return io_windows_status(GetLastError());
    owner->handle = 0;
    return XR_OS_IO_OK;
}
static XrOsIoStatus metadata_finish(XrOsFileMetadataOwner *owner, XrOsIoStatus status) {
    XrOsIoStatus closed = xr_os_io_file_metadata_close(owner);
    return status == XR_OS_IO_OK ? closed : status;
}
static bool metadata_windows_refused(DWORD error) {
    return xr_win_path_error_refused(error) || error == ERROR_CANT_ACCESS_FILE;
}
static XrOsIoStatus metadata_open(const wchar_t *path, DWORD flags, XrOsFileMetadataOwner *owner,
    bool *present) {
    HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | flags, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (metadata_windows_refused(error)) { *present = false; return XR_OS_IO_OK; }
        return io_windows_status(error);
    }
    owner->handle = (intptr_t)handle;
    *present = true;
    return XR_OS_IO_OK;
}
static int64_t metadata_seconds(FILETIME time) {
    uint64_t ticks = ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
    uint64_t seconds = ticks / 10000000;
    const uint64_t epoch = UINT64_C(11644473600);
    return seconds >= epoch ? (int64_t)(seconds - epoch) : -(int64_t)(epoch - seconds);
}
static bool metadata_executable_name(const wchar_t *path) {
    size_t length = wcslen(path);
    if (length < 4 || path[length - 4] != L'.') return false;
    wchar_t suffix[3];
    for (size_t i = 0; i < 3; ++i) {
        wchar_t value = path[length - 3 + i];
        suffix[i] = value >= L'A' && value <= L'Z' ? value + (L'a' - L'A') : value;
    }
    return !memcmp(suffix, L"exe", sizeof(suffix)) || !memcmp(suffix, L"com", sizeof(suffix)) ||
        !memcmp(suffix, L"bat", sizeof(suffix)) || !memcmp(suffix, L"cmd", sizeof(suffix));
}
static XrOsIoStatus metadata_follow(const wchar_t *wide, bool (*current)(void *), void *context,
    XrOsFileMetadataOwner *owner, XrOsFileMetadata *result) {
    if (current && !current(context)) return XR_OS_IO_CANCELLED;
    XrOsIoStatus status = metadata_open(wide, 0, owner, &result->present);
    BY_HANDLE_FILE_INFORMATION data;
    if (status == XR_OS_IO_OK && result->present) {
        if (!GetFileInformationByHandle((HANDLE)owner->handle, &data)) status = io_windows_status(GetLastError());
        else {
            SetLastError(ERROR_SUCCESS);
            DWORD file_type = GetFileType((HANDLE)owner->handle);
            DWORD type_error = file_type == FILE_TYPE_UNKNOWN ? GetLastError() : ERROR_SUCCESS;
            uint64_t size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
            if (type_error != ERROR_SUCCESS) status = io_windows_status(type_error);
            else if (size > INT64_MAX) status = XR_OS_IO_OVERFLOW;
            else {
                result->size = (int64_t)size;
                result->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                result->is_file = !result->is_dir && file_type == FILE_TYPE_DISK;
                result->mode = _S_IREAD | ((data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0 : _S_IWRITE);
                if (result->is_dir || metadata_executable_name(wide)) result->mode |= _S_IEXEC;
                result->mode &= 0777;
                result->mtime = metadata_seconds(data.ftLastWriteTime);
                result->atime = metadata_seconds(data.ftLastAccessTime);
                result->ctime = metadata_seconds(data.ftCreationTime);
            }
        }
    }
    return metadata_finish(owner, status);
}

XR_FUNC XrOsIoStatus xr_os_io_file_metadata(const XrOsIoPolicy *policy,
    const char *path, size_t length, bool (*current)(void *context),
    XrOsFileMetadataOwner *owner, XrOsFileMetadata *output) {
    if (!io_policy_valid(policy) || !owner || owner->handle || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    char *terminated = NULL;
    wchar_t *wide = NULL;
    XrOsFileMetadata result = {0};
    io_status(&io, metadata_path_owned(policy, path, length, current, &terminated));
    bool refused = false;
    /* Validation above reaches the entire span. A UTF-16 unit has at most
     * three UTF-8 bytes, so this valid input cannot fit the locator ceiling.
     * Bound the old C-string conversion scans without skipping invalid tails. */
    if (io.status == XR_OS_IO_OK && length > (size_t)32767 * 3) refused = true;
    if (io.status == XR_OS_IO_OK && !refused) {
        io_status(&io, xr_win_utf8_path_owned_classified(policy, terminated, &wide, &refused));
        if (refused) io.status = XR_OS_IO_OK;
    }
    if (io.status == XR_OS_IO_OK && current && !current(policy->context)) io.status = XR_OS_IO_CANCELLED;
    if (io.status == XR_OS_IO_OK && !refused) {
        DWORD attributes = GetFileAttributesW(wide);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if (!metadata_windows_refused(error)) io_status(&io, io_windows_status(error));
        } else {
            result.present = true;
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                io_status(&io, metadata_open(wide, FILE_FLAG_OPEN_REPARSE_POINT, owner, &result.present));
                if (io.status == XR_OS_IO_OK && result.present) {
                    FILE_ATTRIBUTE_TAG_INFO tag;
                    if (!GetFileInformationByHandleEx((HANDLE)owner->handle, FileAttributeTagInfo, &tag, sizeof(tag)))
                        io_status(&io, io_windows_status(GetLastError()));
                    else result.is_symlink = IsReparseTagNameSurrogate(tag.ReparseTag) != 0;
                }
                io.status = metadata_finish(owner, io.status);
            }
            if (io.status == XR_OS_IO_OK && result.present)
                io_status(&io, metadata_follow(wide, current, policy->context, owner, &result));
        }
    }
    io_free(&io, wide);
    io_free(&io, terminated);
    if (io.status == XR_OS_IO_OK && current && !current(policy->context)) io.status = XR_OS_IO_CANCELLED;
    if (io.status == XR_OS_IO_OK) *output = result;
    return io.status;
}
