/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfileio.c - File I/O and path utilities implementation
 */

#include "xfileio.h"
#include "xmalloc.h"
#include "xchecks.h"
#include "xio_policy.inc.h"
#include "../shared/xr_path_limit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef XR_OS_WINDOWS
#include "xwindows_utf8.h"
#else
#include <sys/stat.h>
#endif

XR_FUNC XrOsIoStatus xr_file_probe_owned(const XrOsIoPolicy *policy, const char *path, bool allow_links) {
    if (!io_policy_valid(policy) || !path || !path[0]) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
#ifdef XR_OS_WINDOWS
    wchar_t *wide = NULL;
    io_status(&io, xr_win_utf8_path_owned(policy, path, &wide));
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!io_work(&io, 1)) { io_free(&io, wide); return io.status; }
    BOOL found = GetFileAttributesExW(wide, GetFileExInfoStandard, &attributes);
    DWORD error = found ? ERROR_SUCCESS : GetLastError();
    io_free(&io, wide);
    if (!found) return io_windows_status(error);
    if (attributes.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) return XR_OS_IO_BAD_ARGUMENT;
    return !allow_links && (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? XR_OS_IO_BAD_ARGUMENT : XR_OS_IO_OK;
#else
    size_t length = 0;
    if (!io_length(&io, path, &length) || !io_work(&io, 1)) return io.status;
    struct stat attributes;
    if (lstat(path, &attributes)) return io_errno_status(errno);
    return S_ISREG(attributes.st_mode) || (allow_links && S_ISLNK(attributes.st_mode)) ? XR_OS_IO_OK : XR_OS_IO_BAD_ARGUMENT;
#endif
}

char *xr_file_read_all(const char *path, const char *mode, size_t *out_size) {
    if (!path || !mode)
        return NULL;

#ifdef XR_OS_WINDOWS
    XrOsIoPolicy policy = xr_os_io_system_policy();
    wchar_t *wide_path = NULL, *wide_mode = NULL;
    (void)xr_win_utf8_path_owned(&policy, path, &wide_path);
    if (wide_path) (void)xr_win_utf8_path_owned(&policy, mode, &wide_mode);
    FILE *f = wide_path && wide_mode ? _wfopen(wide_path, wide_mode) : NULL;
    xr_free(wide_mode); xr_free(wide_path);
#else
    FILE *f = fopen(path, mode);
#endif
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    XR_DCHECK(size >= 0, "xr_file_read_all: ftell returned negative");
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return NULL;
    }

    char *buf = (char *) xr_malloc((size_t) size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t) size, f);
    buf[n] = '\0';
    fclose(f);

    if (out_size)
        *out_size = n;
    return buf;
}

static bool path_separator(char c) {
    return c == '/'
#ifdef XR_OS_WINDOWS
        || c == '\\'
#endif
        ;
}
/* Root recognition is lexical. It grants no filesystem authority. */
static size_t path_root_length(XrIoContext *io, const char *path, size_t length) {
    if (!length || !io_work(io, 1)) return 0;
    bool rooted = path_separator(path[0]);
#ifdef XR_OS_WINDOWS
    if (length >= 2 && io_work(io, 1)) {
        char second = path[1];
        if (!rooted && second == ':' && length >= 3 && io_work(io, 1))
            return path_separator(path[2]) ? 3 : 0;
        if (rooted && path_separator(second)) {
            size_t offset = 2;
            for (unsigned component = 0; component < 2; ++component) {
                size_t begin = offset;
                while (offset < length && io_work(io, 1) && !path_separator(path[offset])) ++offset;
                if (offset == begin) return 1;
                if (component == 0 && offset < length) ++offset;
            }
            return offset;
        }
    }
#endif
    return rooted ? 1 : 0;
}
XR_FUNC XrOsIoStatus xr_path_dirname_owned(const XrOsIoPolicy *policy, const char *path, char **output) {
    if (!io_policy_valid(policy) || !path || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    size_t length = 0;
    if (!io_length(&io, path, &length)) return io.status;
    size_t root = path_root_length(&io, path, length);
    while (length > root && io_work(&io, 1) && path_separator(path[length - 1])) --length;
    while (length > root && io_work(&io, 1) && !path_separator(path[length - 1])) --length;
    while (length > root && io_work(&io, 1) && path_separator(path[length - 1])) --length;
    const char *source = path;
    if (!length) { source = "."; length = 1; }
    char *result = io_alloc(&io, length + 1);
    if (result && io_copy(&io, result, source, length) && io_work(&io, 1)) result[length] = 0;
    if (io.status == XR_OS_IO_OK) *output = result;
    else io_free(&io, result);
    return io.status;
}

XR_FUNC XrOsIoStatus xr_path_join_owned(const XrOsIoPolicy *policy, const char *dir,
    const char *name, char **output) {
    if (!io_policy_valid(policy) || !dir || !name || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK}; size_t dl = 0, nl = 0;
    if (!io_length(&io, dir, &dl) || !io_length(&io, name, &nl)) return io.status;
    if (dl > 1 && io_work(&io, 1) && path_separator(dir[dl - 1])) {
        size_t root = path_root_length(&io, dir, dl);
        while (dl > root && io_work(&io, 1) && path_separator(dir[dl - 1])) --dl;
    }
    bool separator = dl != 0;
    if (separator && io_work(&io, 1) && path_separator(dir[dl - 1])) separator = false;
    if (nl > SIZE_MAX - 2 || dl > SIZE_MAX - nl - 2) return XR_OS_IO_BUDGET;
    size_t size = dl + (separator ? 1u : 0u) + nl + 1;
    char *result = io_alloc(&io, size);
    if (result && io_copy(&io, result, dir, dl)) {
        size_t offset = dl;
        if (separator && io_work(&io, 1)) result[offset++] = '/';
        if (io_copy(&io, result + offset, name, nl + 1)) *output = result;
    }
    if (io.status != XR_OS_IO_OK) io_free(&io, result);
    return io.status;
}

char *xr_path_basename(const char *path) {
    if (!path || !*path)
        return xr_strdup(".");

    size_t len = strlen(path);

    /* Strip trailing slashes */
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    /* Root "/" */
    if (len == 1 && path[0] == '/')
        return xr_strdup("/");

    const char *end = path + len;
    const char *start = end;
    while (start > path && *(start - 1) != '/') {
        start--;
    }

    size_t blen = (size_t) (end - start);
    char *result = (char *) xr_malloc(blen + 1);
    if (!result)
        return NULL;

    memcpy(result, start, blen);
    result[blen] = '\0';
    return result;
}

XR_FUNC XrOsIoStatus xr_realpath_owned(const XrOsIoPolicy *policy, const char *path, char **output) {
    if (!io_policy_valid(policy) || !path || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
#ifdef XR_OS_WINDOWS
    wchar_t *wide = NULL, *resolved = NULL;
    io_status(&io, xr_win_utf8_path_owned(policy, path, &wide));
    DWORD units = 0;
    if (io_work(&io, 1)) {
        units = GetFullPathNameW(wide, 0, NULL, NULL);
        if (!units || units > 32768) io_status(&io, units ? XR_OS_IO_BUDGET : io_windows_status(GetLastError()));
    }
    if (io.status == XR_OS_IO_OK) resolved = io_alloc(&io, (size_t)units * sizeof(wchar_t));
    if (resolved && io_work(&io, 1 + (size_t)units * sizeof(wchar_t))) {
        DWORD length = GetFullPathNameW(wide, units, resolved, NULL);
        if (!length || length >= units) io_status(&io, length ? XR_OS_IO_BUDGET : io_windows_status(GetLastError()));
    }
    char *result = NULL;
    if (io.status == XR_OS_IO_OK) io_status(&io, xr_win_utf16_text_owned(policy, resolved, &result));
    io_free(&io, resolved); io_free(&io, wide);
#else
    size_t length = 0;
    if (!io_length(&io, path, &length)) return io.status;
    /* Supplying the output storage avoids realpath(NULL)'s hidden allocation.
     * POSIX realpath requires room for PATH_MAX, including its terminator. */
    char *result = io_alloc(&io, XR_PATH_LIMIT_MAX_PATH);
    if (result && io_work(&io, 1 + length + XR_PATH_LIMIT_MAX_PATH) && !realpath(path, result))
        io_status(&io, io_errno_status(errno));
    if (io.status != XR_OS_IO_OK) { io_free(&io, result); result = NULL; }
#endif
    if (io.status == XR_OS_IO_OK) *output = result;
    return io.status;
}
