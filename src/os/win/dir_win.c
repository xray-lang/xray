/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * dir_win.c - One UTF-8 directory iterator with explicit allocation/work policy
 */
#include "../os_dir.h"
#include "../../base/xwindows_utf8.h"

struct XrDirIter {
    XrOsIoPolicy policy;
    HANDLE handle;
    WIN32_FIND_DATAW data;
    XrDirEntry pending;
    XrOsIoStatus status;
    bool primed, ended;
};

XR_FUNC XrOsIoStatus xr_os_io_dir_open(const XrOsIoPolicy *policy, const char *path, XrDirIter **output) {
    if (!io_policy_valid(policy) || !path || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; size_t length = 0;
    if (!io_length(&io, path, &length)) return io.status;
    if (!length) return XR_OS_IO_BAD_ARGUMENT;
    if (length > SIZE_MAX - 3) return XR_OS_IO_BUDGET;
    XrDirIter *iterator = io_alloc(&io, sizeof(*iterator));
    if (!iterator) return io.status;
    if (!io_clear(&io, iterator, sizeof(*iterator))) { io_free(&io, iterator); return io.status; }
    iterator->policy = *policy; iterator->handle = INVALID_HANDLE_VALUE;
    char *pattern = io_alloc(&io, length + 3); wchar_t *wide = NULL;
    if (pattern && io_copy(&io, pattern, path, length) && io_work(&io, 1)) {
        bool separator = path[length - 1] != '/' && path[length - 1] != 92;
        if (separator && io_work(&io, 1)) pattern[length++] = 92;
        if (io_work(&io, 2)) { pattern[length++] = '*'; pattern[length] = 0; }
    }
    if (io.status == XR_OS_IO_OK) io_status(&io, xr_win_utf8_path_owned(policy, pattern, &wide));
    if (io_work(&io, 1)) {
        iterator->handle = FindFirstFileW(wide, &iterator->data);
        if (iterator->handle == INVALID_HANDLE_VALUE) io_status(&io, io_windows_status(GetLastError()));
        else iterator->primed = true;
    }
    io_free(&io, wide); io_free(&io, pattern);
    if (io.status != XR_OS_IO_OK) xr_os_io_dir_close(iterator);
    else *output = iterator;
    return io.status;
}

XR_FUNC XrOsIoStatus xr_os_io_dir_next(XrDirIter *iterator, XrDirEntry *output) {
    if (!iterator || !output) return XR_OS_IO_BAD_ARGUMENT;
    if (iterator->status != XR_OS_IO_OK) return iterator->status;
    if (iterator->ended) return XR_OS_IO_END;
    XrIoContext io = {&iterator->policy, XR_OS_IO_OK};
    while (io.status == XR_OS_IO_OK) {
        if (!iterator->primed && io_work(&io, 1) && !FindNextFileW(iterator->handle, &iterator->data)) {
            DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_FILES) { iterator->ended = true; return XR_OS_IO_END; }
            io_status(&io, io_windows_status(error));
        }
        iterator->primed = false;
        if (io.status != XR_OS_IO_OK) break;
        const wchar_t *name = iterator->data.cFileName;
        bool dot = false;
        if (io_work(&io, sizeof(wchar_t)) && name[0] == '.' && io_work(&io, sizeof(wchar_t))) {
            if (!name[1]) dot = true;
            else if (name[1] == '.' && io_work(&io, sizeof(wchar_t)) && !name[2]) dot = true;
        }
        if (dot) continue;
        char *text = NULL; size_t length = 0;
        if (io.status == XR_OS_IO_OK) io_status(&io, xr_win_utf16_text_owned(io.policy, name, &text));
        if (io.status == XR_OS_IO_OK && io_length(&io, text, &length)) {
            if (length >= XR_DIR_ENTRY_NAME_MAX) io_status(&io, XR_OS_IO_BUDGET);
            else if (io_clear(&io, &iterator->pending, sizeof(iterator->pending)) &&
                io_copy(&io, iterator->pending.name, text, length + 1)) {
                iterator->pending.is_dir = (iterator->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                io_copy(&io, output, &iterator->pending, sizeof(*output));
            }
        }
        io_free(&io, text); break;
    }
    iterator->status = io.status;
    return io.status;
}
XR_FUNC void xr_os_io_dir_close(XrDirIter *iterator) {
    if (!iterator) return;
    if (iterator->handle != INVALID_HANDLE_VALUE) FindClose(iterator->handle);
    XrOsIoPolicy policy = iterator->policy;
    policy.free(policy.context, iterator);
}
