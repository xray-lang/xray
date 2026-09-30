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
#include "../../base/xmalloc.h"
#include <windows.h>
#include <string.h>
#include <wchar.h>

enum { FILE_PATH_UNITS = 32768, FILE_ANCESTOR_LIMIT = 256 };

static bool logical_path_valid(const char *path) {
    if (!path || !*path) return false;
    const char *segment = path;
    for (const char *p = path;; ++p) {
        unsigned char c = (unsigned char) *p;
        if (!c || c == '/') {
            size_t length = (size_t) (p - segment);
            if (!length || segment[length - 1] == '.' || segment[length - 1] == ' ')
                return false;
            if (!c) return true;
            segment = p + 1;
        } else if (c < 0x20 || c == 0x7F || c == '\\' || c == ':' || c == '?' || c == '*') {
            return false;
        }
    }
}

static wchar_t *wide_path(const char *text, XrFileReadStatus *status) {
    XrWinPathStatus converted;
    wchar_t *result = xr_win_utf8_path(text, &converted);
    if (!result) *status = converted == XR_WIN_PATH_OOM ? XR_FILE_READ_OUT_OF_MEMORY :
        converted == XR_WIN_PATH_LIMIT ? XR_FILE_READ_LIMIT : XR_FILE_READ_FORBIDDEN;
    return result;
}

static bool same_file(const BY_HANDLE_FILE_INFORMATION *a, const BY_HANDLE_FILE_INFORMATION *b) {
    return a->dwVolumeSerialNumber == b->dwVolumeSerialNumber &&
        a->nFileIndexHigh == b->nFileIndexHigh && a->nFileIndexLow == b->nFileIndexLow;
}

static XrFileReadStatus physical_ancestor(HANDLE file, const BY_HANDLE_FILE_INFORMATION *root) {
    wchar_t *path = xr_malloc(FILE_PATH_UNITS * sizeof(wchar_t));
    if (!path) return XR_FILE_READ_OUT_OF_MEMORY;
    DWORD length = GetFinalPathNameByHandleW(file, path, FILE_PATH_UNITS, FILE_NAME_NORMALIZED);
    XrFileReadStatus status = XR_FILE_READ_FORBIDDEN;
    if (!length) { status = XR_FILE_READ_IO; goto done; }
    if (length >= FILE_PATH_UNITS) { status = XR_FILE_READ_LIMIT; goto done; }
    for (unsigned depth = 0; depth < FILE_ANCESTOR_LIMIT; ++depth) {
        wchar_t *slash = wcsrchr(path, L'\\');
        if (!slash || slash == path) goto done;
        bool drive_root = slash > path && slash[-1] == L':';
        if (drive_root) slash[1] = 0;
        else *slash = 0;
        HANDLE parent = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (parent == INVALID_HANDLE_VALUE) { status = XR_FILE_READ_IO; goto done; }
        BY_HANDLE_FILE_INFORMATION information;
        BOOL read = GetFileInformationByHandle(parent, &information);
        CloseHandle(parent);
        if (!read) { status = XR_FILE_READ_IO; goto done; }
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && same_file(root, &information)) {
            status = XR_FILE_READ_OK; goto done;
        }
        if (drive_root || information.dwVolumeSerialNumber != root->dwVolumeSerialNumber) goto done;
    }
    status = XR_FILE_READ_LIMIT;
done:
    xr_free(path);
    return status;
}

static XrFileReadStatus read_bytes(HANDLE file, size_t limit, XrFileBytes *output) {
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) return XR_FILE_READ_IO;
    if ((uint64_t) size.QuadPart > limit || (uint64_t) size.QuadPart >= SIZE_MAX)
        return XR_FILE_READ_LIMIT;
    size_t length = (size_t) size.QuadPart, offset = 0;
    char *data = xr_malloc(length + 1);
    if (!data) return XR_FILE_READ_OUT_OF_MEMORY;
    while (offset < length) {
        size_t remaining = length - offset;
        DWORD count = remaining > 65536 ? 65536 : (DWORD) remaining, got = 0;
        if (!ReadFile(file, data + offset, count, &got, NULL) || !got) goto fail;
        offset += got;
    }
    char extra;
    DWORD got = 0;
    if (!ReadFile(file, &extra, 1, &got, NULL) || got) goto fail;
    LARGE_INTEGER after;
    if (!GetFileSizeEx(file, &after) || after.QuadPart != size.QuadPart) goto fail;
    data[length] = 0; output->data = data; output->size = length;
    return XR_FILE_READ_OK;
fail:
    xr_free(data); return XR_FILE_READ_IO;
}

/* Absence is meaningful only after the nearest existing parent is authorized. */
static XrFileReadStatus missing_target_status(wchar_t *path, const BY_HANDLE_FILE_INFORMATION *root) {
    for (unsigned depth = 0; depth < FILE_ANCESTOR_LIMIT; ++depth) {
        wchar_t *slash = wcsrchr(path, L'\\');
        if (!slash || slash == path) return XR_FILE_READ_FORBIDDEN;
        bool drive_root = slash > path && slash[-1] == L':';
        if (drive_root) slash[1] = 0;
        else *slash = 0;
        HANDLE parent = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (parent == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (drive_root || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND))
                return XR_FILE_READ_IO;
            continue;
        }
        BY_HANDLE_FILE_INFORMATION information;
        XrFileReadStatus status = XR_FILE_READ_FORBIDDEN;
        if (!GetFileInformationByHandle(parent, &information)) status = XR_FILE_READ_IO;
        else if (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            status = same_file(root, &information) ? XR_FILE_READ_OK : physical_ancestor(parent, root);
            if (status == XR_FILE_READ_OK) status = XR_FILE_READ_MISSING;
        }
        CloseHandle(parent);
        return status;
    }
    return XR_FILE_READ_LIMIT;
}

XR_FUNC XrFileReadStatus xr_file_read_under_root(const char *root, const char *logical_path,
    size_t limit, XrFileBytes *output) {
    if (!output) return XR_FILE_READ_FORBIDDEN;
    *output = (XrFileBytes){0};
    if (!root || !*root || !logical_path_valid(logical_path)) return XR_FILE_READ_FORBIDDEN;
    XrFileReadStatus status = XR_FILE_READ_FORBIDDEN;
    wchar_t *directory = wide_path(root, &status), *relative = NULL, *combined = NULL;
    HANDLE root_handle = INVALID_HANDLE_VALUE, file = INVALID_HANDLE_VALUE;
    if (!directory) goto done;
    size_t root_length = wcslen(directory);
    bool absolute = root_length >= 3 && ((directory[1] == L':' &&
        (directory[2] == L'/' || directory[2] == L'\\')) ||
        (directory[0] == L'\\' && directory[1] == L'\\'));
    if (!absolute) goto done;
    relative = wide_path(logical_path, &status);
    if (!relative) goto done;
    size_t logical_length = wcslen(relative);
    if (root_length + logical_length + 2 > FILE_PATH_UNITS) { status = XR_FILE_READ_LIMIT; goto done; }
    combined = xr_malloc((root_length + logical_length + 2) * sizeof(wchar_t));
    if (!combined) { status = XR_FILE_READ_OUT_OF_MEMORY; goto done; }
    memcpy(combined, directory, root_length * sizeof(wchar_t));
    combined[root_length] = L'\\';
    memcpy(combined + root_length + 1, relative, (logical_length + 1) * sizeof(wchar_t));
    for (wchar_t *p = combined; *p; ++p) if (*p == L'/') *p = L'\\';
    root_handle = CreateFileW(directory, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (root_handle == INVALID_HANDLE_VALUE) { status = XR_FILE_READ_IO; goto done; }
    BY_HANDLE_FILE_INFORMATION root_info, file_info;
    if (!GetFileInformationByHandle(root_handle, &root_info)) { status = XR_FILE_READ_IO; goto done; }
    if (!(root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) goto done;
    file = CreateFileW(combined, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        status = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
            missing_target_status(combined, &root_info) : XR_FILE_READ_IO;
        goto done;
    }
    if (GetFileType(file) != FILE_TYPE_DISK) goto done;
    if (!GetFileInformationByHandle(file, &file_info)) { status = XR_FILE_READ_IO; goto done; }
    if (file_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) goto done;
    status = physical_ancestor(file, &root_info);
    if (status == XR_FILE_READ_OK) status = read_bytes(file, limit, output);
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (root_handle != INVALID_HANDLE_VALUE) CloseHandle(root_handle);
    xr_free(combined); xr_free(relative); xr_free(directory);
    return status;
}
