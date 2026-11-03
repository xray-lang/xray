/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_mutation_win.c - Wide path mutations with final-write cancellation
 */
#include "../os_file_mutation.h"
#include "../file_metadata_path.inc.h"
#include "../../base/xwindows_utf8.h"

static XrOsIoStatus file_mutation_locator(const XrOsIoPolicy *policy, XrOsPathSpan path,
    bool (*current)(void *), wchar_t **output, bool *refused) {
    char *terminated = NULL;
    XrOsIoStatus status = metadata_path_owned(policy, path.text, path.length, current, &terminated);
    *refused = false;
    if (status != XR_OS_IO_OK) return status;
    /* Full UTF validation precedes this UTF-16 capacity consequence. */
    if (path.length > (size_t)32767 * 3) *refused = true;
    else {
        status = xr_win_utf8_path_owned_classified(policy, terminated, output, refused);
        if (*refused) status = XR_OS_IO_OK;
    }
    policy->free(policy->context, terminated);
    if (status == XR_OS_IO_OK && current && !current(policy->context)) status = XR_OS_IO_CANCELLED;
    return status;
}
static bool file_mutation_refused(DWORD error) {
    return xr_win_path_error_refused(error) || error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ||
        error == ERROR_DIR_NOT_EMPTY || error == ERROR_NOT_SAME_DEVICE || error == ERROR_CANT_ACCESS_FILE ||
        error == ERROR_CANNOT_DELETE || error == ERROR_DELETE_PENDING || error == ERROR_DISK_FULL ||
        error == ERROR_HANDLE_DISK_FULL;
}
static XrOsIoStatus file_mutation_error(DWORD error) {
    return file_mutation_refused(error) ? XR_OS_IO_OK : io_windows_status(error);
}
static XrOsIoStatus file_mutation_closed(XrOsFileMetadataOwner *owner, XrOsIoStatus status) {
    XrOsIoStatus closed = xr_os_io_file_metadata_close(owner);
    return status == XR_OS_IO_OK ? closed : status;
}
static bool file_mutation_current(const XrOsIoPolicy *policy, bool (*current)(void *), XrIoContext *io) {
    return !current || current(policy->context) || io_status(io, XR_OS_IO_CANCELLED);
}

static XrOsIoStatus file_mutation_directory(const XrOsIoPolicy *policy, XrOsPathSpan path,
    bool (*current)(void *), bool create, bool *output) {
    if (!io_policy_valid(policy) || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    wchar_t *wide = NULL; bool refused = false, changed = false;
    io_status(&io, file_mutation_locator(policy, path, current, &wide, &refused));
    if (io.status == XR_OS_IO_OK && !refused && file_mutation_current(policy, current, &io)) {
        changed = create ? CreateDirectoryW(wide, NULL) != 0 : RemoveDirectoryW(wide) != 0;
        if (!changed) {
            DWORD error = GetLastError();
            if (create && (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)) {
                DWORD attributes = GetFileAttributesW(wide);
                if (attributes != INVALID_FILE_ATTRIBUTES) changed = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                else io_status(&io, file_mutation_error(GetLastError()));
            } else io_status(&io, file_mutation_error(error));
        }
    }
    io_free(&io, wide);
    if (io.status == XR_OS_IO_OK) *output = changed;
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_path_mkdir(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *), bool *output) {
    return file_mutation_directory(policy, path, current, true, output);
}
XR_FUNC XrOsIoStatus xr_os_io_path_rmdir(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *), bool *output) {
    return file_mutation_directory(policy, path, current, false, output);
}
XR_FUNC XrOsIoStatus xr_os_io_path_rename(const XrOsIoPolicy *policy,
    XrOsPathSpan old_path, XrOsPathSpan new_path, bool (*current)(void *), bool *output) {
    if (!io_policy_valid(policy) || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    wchar_t *from = NULL, *to = NULL;
    bool from_refused = false, to_refused = false, changed = false;
    io_status(&io, file_mutation_locator(policy, old_path, current, &from, &from_refused));
    if (io.status == XR_OS_IO_OK)
        io_status(&io, file_mutation_locator(policy, new_path, current, &to, &to_refused));
    if (io.status == XR_OS_IO_OK && !from_refused && !to_refused && file_mutation_current(policy, current, &io)) {
        changed = MoveFileExW(from, to, 0) != 0;
        if (!changed) io_status(&io, file_mutation_error(GetLastError()));
    }
    io_free(&io, to); io_free(&io, from);
    if (io.status == XR_OS_IO_OK) *output = changed;
    return io.status;
}

static bool file_mutation_remove_handle(const XrOsIoPolicy *policy, bool (*current)(void *),
    XrOsFileMetadataOwner *owner, XrIoContext *io) {
    FILE_ATTRIBUTE_TAG_INFO tag;
    HANDLE handle = (HANDLE)owner->handle;
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)))
        return io_status(io, io_windows_status(GetLastError())) && false;
    if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !IsReparseTagNameSurrogate(tag.ReparseTag)) return false;
    if (tag.FileAttributes & FILE_ATTRIBUTE_READONLY) {
        FILE_BASIC_INFO basic = {0};
        basic.FileAttributes = tag.FileAttributes & ~FILE_ATTRIBUTE_READONLY;
        if (!basic.FileAttributes) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!file_mutation_current(policy, current, io)) return false;
        if (!SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic))) {
            io_status(io, file_mutation_error(GetLastError()));
            return false;
        }
    }
    if (!file_mutation_current(policy, current, io)) return false;
    FILE_DISPOSITION_INFO disposition = {TRUE};
    if (!SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition))) {
        io_status(io, file_mutation_error(GetLastError()));
        return false;
    }
    return true;
}
XR_FUNC XrOsIoStatus xr_os_io_path_remove(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *), XrOsFileMetadataOwner *owner, bool *output) {
    if (!io_policy_valid(policy) || !owner || owner->handle || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    wchar_t *wide = NULL; bool refused = false, changed = false;
    io_status(&io, file_mutation_locator(policy, path, current, &wide, &refused));
    if (io.status == XR_OS_IO_OK && !refused && file_mutation_current(policy, current, &io)) {
        DWORD attributes = GetFileAttributesW(wide);
        if (attributes == INVALID_FILE_ATTRIBUTES) io_status(&io, file_mutation_error(GetLastError()));
        else {
            DWORD access = DELETE | FILE_READ_ATTRIBUTES;
            if (attributes & FILE_ATTRIBUTE_READONLY) access |= FILE_WRITE_ATTRIBUTES;
            HANDLE handle = CreateFileW(wide, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
            if (handle == INVALID_HANDLE_VALUE) io_status(&io, file_mutation_error(GetLastError()));
            else {
                owner->handle = (intptr_t)handle;
                changed = file_mutation_remove_handle(policy, current, owner, &io);
            }
        }
    }
    io_free(&io, wide);
    io.status = file_mutation_closed(owner, io.status);
    if (io.status == XR_OS_IO_OK) *output = changed;
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_path_utime_now(const XrOsIoPolicy *policy,
    XrOsPathSpan path, bool (*current)(void *), XrOsFileMetadataOwner *owner, bool *output) {
    if (!io_policy_valid(policy) || !owner || owner->handle || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    wchar_t *wide = NULL; bool refused = false, changed = false;
    io_status(&io, file_mutation_locator(policy, path, current, &wide, &refused));
    if (io.status == XR_OS_IO_OK && !refused && file_mutation_current(policy, current, &io)) {
        HANDLE handle = CreateFileW(wide, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (handle == INVALID_HANDLE_VALUE) io_status(&io, file_mutation_error(GetLastError()));
        else {
            owner->handle = (intptr_t)handle;
            FILETIME now; GetSystemTimeAsFileTime(&now);
            if (file_mutation_current(policy, current, &io)) {
                changed = SetFileTime(handle, NULL, &now, &now) != 0;
                if (!changed) io_status(&io, file_mutation_error(GetLastError()));
            }
        }
    }
    io_free(&io, wide);
    io.status = file_mutation_closed(owner, io.status);
    if (io.status == XR_OS_IO_OK) *output = changed;
    return io.status;
}
