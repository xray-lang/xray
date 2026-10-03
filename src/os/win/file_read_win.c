/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_read_win.c - Handle-bound physical ancestry and bounded UTF-8 file reads
 *
 * KEY CONCEPT:
 *   Directory identities, not case-folded path prefixes, establish ancestry.
 */
#include "../os_file_read.h"
#include "../../base/xwindows_utf8.h"
#include <windows.h>
#include <string.h>

enum { FILE_PATH_UNITS = 32768, FILE_ANCESTOR_LIMIT = 256 };

static XrFileReadStatus file_io_status(XrOsIoStatus status) {
    switch (status) {
    case XR_OS_IO_OK: return XR_FILE_READ_OK;
    case XR_OS_IO_BUDGET: return XR_FILE_READ_LIMIT;
    case XR_OS_IO_OUT_OF_MEMORY: return XR_FILE_READ_OUT_OF_MEMORY;
    case XR_OS_IO_BAD_ARGUMENT: return XR_FILE_READ_FORBIDDEN;
    default: return XR_FILE_READ_IO;
    }
}
static bool logical_path_valid(XrIoContext *io, const char *path) {
    const char *segment = path;
    for (const char *p = path; io_work(io, 1); ++p) {
        unsigned char c = (unsigned char)*p;
        if (!c || c == '/') {
            size_t length = (size_t)(p - segment);
            if (!length || !io_work(io, 1)) return false;
            char last = segment[length - 1];
            if (last == '.' || last == ' ') return false;
            if (!c) return true;
            segment = p + 1;
        } else if (c < 0x20 || c == 0x7F || c == '\\' || c == ':' || c == '?' || c == '*') {
            return false;
        }
    }
    return false;
}
static bool wide_length(XrIoContext *io, const wchar_t *text, size_t *length) {
    size_t n = 0;
    while (io_work(io, sizeof(wchar_t))) {
        if (!text[n]) { *length = n; return true; }
        if (++n == FILE_PATH_UNITS) return io_status(io, XR_OS_IO_BUDGET);
    }
    return false;
}
/* The whole prefix is traversed because the final separator determines the parent. */
static wchar_t *last_separator(XrIoContext *io, wchar_t *text, size_t *length) {
    wchar_t *last = NULL;
    for (size_t n = 0; io_work(io, sizeof(wchar_t)); ++n) {
        wchar_t c = text[n];
        if (!c) { *length = n; return last; }
        if (c == L'\\') last = text + n;
    }
    return NULL;
}
static bool same_file(XrIoContext *io, const BY_HANDLE_FILE_INFORMATION *a,
    const BY_HANDLE_FILE_INFORMATION *b) {
    if (!io_work(io, 6 * sizeof(DWORD))) return false;
    return a->dwVolumeSerialNumber == b->dwVolumeSerialNumber &&
        a->nFileIndexHigh == b->nFileIndexHigh && a->nFileIndexLow == b->nFileIndexLow;
}
static HANDLE open_directory(XrIoContext *io, const wchar_t *path, size_t length) {
    if (!io_work(io, 1 + (length + 1) * sizeof(wchar_t))) return INVALID_HANDLE_VALUE;
    return CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
}
static bool file_information(XrIoContext *io, HANDLE file, BY_HANDLE_FILE_INFORMATION *information) {
    return io_work(io, 1 + sizeof(*information)) && GetFileInformationByHandle(file, information);
}
static wchar_t *parent_path(XrIoContext *io, wchar_t *path, size_t *length, bool *drive_root) {
    wchar_t *slash = last_separator(io, path, length);
    if (!slash || slash == path || !io_work(io, sizeof(wchar_t))) return NULL;
    *drive_root = slash[-1] == L':';
    *length = (size_t)(slash - path) + (*drive_root ? 1u : 0u);
    if (!io_work(io, sizeof(wchar_t))) return NULL;
    path[*length] = 0;
    return slash;
}
static XrFileReadStatus physical_ancestor(XrIoContext *io, HANDLE file,
    const BY_HANDLE_FILE_INFORMATION *root) {
    wchar_t *path = io_alloc(io, FILE_PATH_UNITS * sizeof(wchar_t));
    if (!path) return file_io_status(io->status);
    XrFileReadStatus status = XR_FILE_READ_FORBIDDEN;
    if (!io_work(io, 1 + FILE_PATH_UNITS * sizeof(wchar_t))) goto done;
    DWORD length = GetFinalPathNameByHandleW(file, path, FILE_PATH_UNITS, FILE_NAME_NORMALIZED);
    if (!length) { status = XR_FILE_READ_IO; goto done; }
    if (length >= FILE_PATH_UNITS) { status = XR_FILE_READ_LIMIT; goto done; }
    for (unsigned depth = 0; depth < FILE_ANCESTOR_LIMIT; ++depth) {
        size_t parent_length = 0; bool drive_root = false;
        if (!parent_path(io, path, &parent_length, &drive_root)) goto done;
        HANDLE parent = open_directory(io, path, parent_length);
        if (parent == INVALID_HANDLE_VALUE) { status = XR_FILE_READ_IO; goto done; }
        BY_HANDLE_FILE_INFORMATION information;
        bool read = file_information(io, parent, &information);
        CloseHandle(parent);
        if (!read) { status = XR_FILE_READ_IO; goto done; }
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && same_file(io, root, &information)) {
            status = XR_FILE_READ_OK; goto done;
        }
        if (io->status != XR_OS_IO_OK || drive_root ||
            information.dwVolumeSerialNumber != root->dwVolumeSerialNumber) goto done;
    }
    status = XR_FILE_READ_LIMIT;
done:
    io_free(io, path);
    return io->status == XR_OS_IO_OK ? status : file_io_status(io->status);
}
static XrFileReadStatus read_bytes(XrIoContext *io, HANDLE file, size_t limit, XrFileBytes *output) {
    LARGE_INTEGER size;
    if (!io_work(io, 1 + sizeof(size))) return file_io_status(io->status);
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) return XR_FILE_READ_IO;
    if ((uint64_t)size.QuadPart > limit || (uint64_t)size.QuadPart >= SIZE_MAX)
        return XR_FILE_READ_LIMIT;
    size_t length = (size_t)size.QuadPart, offset = 0;
    char *data = io_alloc(io, length + 1);
    if (!data) return file_io_status(io->status);
    while (offset < length) {
        size_t remaining = length - offset;
        DWORD count = remaining > 65536 ? 65536 : (DWORD)remaining, got = 0;
        if (!io_work(io, 1 + count)) goto fail;
        if (!ReadFile(file, data + offset, count, &got, NULL) || !got) goto fail;
        offset += got;
    }
    char extra; DWORD got = 0;
    if (!io_work(io, 2)) goto fail;
    if (!ReadFile(file, &extra, 1, &got, NULL) || got) goto fail;
    LARGE_INTEGER after;
    if (!io_work(io, 1 + sizeof(after))) goto fail;
    if (!GetFileSizeEx(file, &after) || after.QuadPart != size.QuadPart) goto fail;
    if (!io_work(io, 1)) goto fail;
    data[length] = 0;
    *output = (XrFileBytes){data, length};
    return XR_FILE_READ_OK;
fail:
    io_free(io, data);
    return io->status == XR_OS_IO_OK ? XR_FILE_READ_IO : file_io_status(io->status);
}
/* Absence is meaningful only after the nearest existing parent is authorized. */
static XrFileReadStatus missing_target_status(XrIoContext *io, wchar_t *path,
    const BY_HANDLE_FILE_INFORMATION *root) {
    for (unsigned depth = 0; depth < FILE_ANCESTOR_LIMIT; ++depth) {
        size_t length = 0; bool drive_root = false;
        if (!parent_path(io, path, &length, &drive_root)) return XR_FILE_READ_FORBIDDEN;
        HANDLE parent = open_directory(io, path, length);
        if (parent == INVALID_HANDLE_VALUE) {
            if (io->status != XR_OS_IO_OK) return file_io_status(io->status);
            DWORD error = GetLastError();
            if (drive_root || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND))
                return XR_FILE_READ_IO;
            continue;
        }
        BY_HANDLE_FILE_INFORMATION information;
        XrFileReadStatus status = XR_FILE_READ_FORBIDDEN;
        if (!file_information(io, parent, &information)) status = XR_FILE_READ_IO;
        else if (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            status = same_file(io, root, &information) ? XR_FILE_READ_OK :
                io->status == XR_OS_IO_OK ? physical_ancestor(io, parent, root) : file_io_status(io->status);
            if (status == XR_FILE_READ_OK) status = XR_FILE_READ_MISSING;
        }
        CloseHandle(parent);
        return io->status == XR_OS_IO_OK ? status : file_io_status(io->status);
    }
    return XR_FILE_READ_LIMIT;
}
XR_FUNC XrFileReadStatus xr_os_io_read_under_root(const XrOsIoPolicy *policy,
    const char *root, const char *logical_path, size_t limit, XrFileBytes *output) {
    if (!io_policy_valid(policy) || !root || !logical_path || !output) return XR_FILE_READ_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    XrFileReadStatus status = XR_FILE_READ_FORBIDDEN;
    wchar_t *directory = NULL, *relative = NULL, *combined = NULL;
    HANDLE root_handle = INVALID_HANDLE_VALUE, file = INVALID_HANDLE_VALUE;
    if (!logical_path_valid(&io, logical_path)) goto done;
    io_status(&io, xr_win_utf8_text_owned(policy, root, &directory));
    if (!directory) goto done;
    size_t root_length = 0, logical_length = 0;
    if (!wide_length(&io, directory, &root_length)) goto done;
    if (!io_work(&io, 3 * sizeof(wchar_t))) goto done;
    bool absolute = root_length >= 3 && ((directory[1] == L':' &&
        (directory[2] == L'/' || directory[2] == L'\\')) ||
        (directory[0] == L'\\' && directory[1] == L'\\'));
    if (!absolute) goto done;
    io_status(&io, xr_win_utf8_text_owned(policy, logical_path, &relative));
    if (!relative || !wide_length(&io, relative, &logical_length)) goto done;
    size_t units = root_length + logical_length + 2;
    if (units > FILE_PATH_UNITS) { status = XR_FILE_READ_LIMIT; goto done; }
    combined = io_alloc(&io, units * sizeof(wchar_t));
    if (!combined) goto done;
    if (!io_copy(&io, combined, directory, root_length * sizeof(wchar_t)) ||
        !io_work(&io, sizeof(wchar_t))) goto done;
    combined[root_length] = L'\\';
    if (!io_copy(&io, combined + root_length + 1, relative, (logical_length + 1) * sizeof(wchar_t))) goto done;
    for (size_t n = 0; n < units; ++n) {
        if (!io_work(&io, sizeof(wchar_t))) goto done;
        if (combined[n] == L'/') {
            if (!io_work(&io, sizeof(wchar_t))) goto done;
            combined[n] = L'\\';
        }
    }
    if (!io_status(&io, xr_win_path_locator_owned(policy, &combined, units - 1))) goto done;
    size_t combined_length = 0;
    if (!wide_length(&io, combined, &combined_length)) goto done;
    units = combined_length + 1;
    if (!io_status(&io, xr_win_path_locator_owned(policy, &directory, root_length)) ||
        !wide_length(&io, directory, &root_length)) goto done;
    root_handle = open_directory(&io, directory, root_length);
    if (root_handle == INVALID_HANDLE_VALUE) { status = XR_FILE_READ_IO; goto done; }
    BY_HANDLE_FILE_INFORMATION root_info, file_info;
    if (!file_information(&io, root_handle, &root_info)) { status = XR_FILE_READ_IO; goto done; }
    if (!(root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) goto done;
    if (!io_work(&io, 1 + units * sizeof(wchar_t))) goto done;
    file = CreateFileW(combined, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        status = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
            missing_target_status(&io, combined, &root_info) : XR_FILE_READ_IO;
        goto done;
    }
    if (!io_work(&io, 1) || GetFileType(file) != FILE_TYPE_DISK) goto done;
    if (!file_information(&io, file, &file_info)) { status = XR_FILE_READ_IO; goto done; }
    if (file_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) goto done;
    status = physical_ancestor(&io, file, &root_info);
    if (status == XR_FILE_READ_OK) status = read_bytes(&io, file, limit, output);
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (root_handle != INVALID_HANDLE_VALUE) CloseHandle(root_handle);
    io_free(&io, combined); io_free(&io, relative); io_free(&io, directory);
    return io.status == XR_OS_IO_OK ? status : file_io_status(io.status);
}
