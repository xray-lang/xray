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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef XR_OS_WINDOWS
#include "xwindows_utf8.h"
#else
#include <sys/stat.h>
#endif

#ifdef XR_OS_WINDOWS
static XrPathStatus fileio_windows_status(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return XR_PATH_NOT_FOUND;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return XR_PATH_OUT_OF_MEMORY;
    case ERROR_FILENAME_EXCED_RANGE: case ERROR_BUFFER_OVERFLOW: return XR_PATH_BUDGET;
    case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME: case ERROR_INVALID_PARAMETER:
    case ERROR_NO_UNICODE_TRANSLATION: return XR_PATH_INVALID;
    default: return XR_PATH_IO;
    }
}
static XrPathStatus fileio_conversion_status(XrWinPathStatus status) {
    return status == XR_WIN_PATH_OOM ? XR_PATH_OUT_OF_MEMORY :
        status == XR_WIN_PATH_LIMIT ? XR_PATH_BUDGET : XR_PATH_INVALID;
}
#else
static XrPathStatus fileio_errno_status(int error) {
    return error == ENOMEM ? XR_PATH_OUT_OF_MEMORY : error == ENAMETOOLONG ? XR_PATH_BUDGET :
        error == ENOENT || error == ENOTDIR ? XR_PATH_NOT_FOUND :
        error == EINVAL ? XR_PATH_INVALID : XR_PATH_IO;
}
#endif

XR_FUNC XrPathStatus xr_file_probe(const char *path, bool allow_links) {
    if (!path || !path[0]) return XR_PATH_INVALID;
#ifdef XR_OS_WINDOWS
    XrWinPathStatus converted;
    wchar_t *wide = xr_win_utf8_path(path, &converted);
    if (!wide) return fileio_conversion_status(converted);
    WIN32_FILE_ATTRIBUTE_DATA attributes = {0};
    BOOL found = GetFileAttributesExW(wide, GetFileExInfoStandard, &attributes);
    DWORD error = found ? ERROR_SUCCESS : GetLastError();
    xr_free(wide);
    if (!found) return fileio_windows_status(error);
    if (attributes.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) return XR_PATH_INVALID;
    return !allow_links && (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? XR_PATH_INVALID : XR_PATH_OK;
#else
    struct stat attributes;
    if (lstat(path, &attributes)) return fileio_errno_status(errno);
    return S_ISREG(attributes.st_mode) || (allow_links && S_ISLNK(attributes.st_mode)) ? XR_PATH_OK : XR_PATH_INVALID;
#endif
}

char *xr_file_read_all(const char *path, const char *mode, size_t *out_size) {
    if (!path || !mode)
        return NULL;

#ifdef XR_OS_WINDOWS
    XrWinPathStatus converted;
    wchar_t *wide_path = xr_win_utf8_path(path, &converted);
    wchar_t *wide_mode = wide_path ? xr_win_utf8_path(mode, &converted) : NULL;
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

char *xr_path_dirname(const char *path) {
    if (!path)
        return xr_strdup(".");

    const char *last_fwd = strrchr(path, '/');
#ifdef _WIN32
    const char *last_bwd = strrchr(path, '\\');
    const char *last_slash = last_fwd;
    if (!last_slash || (last_bwd && last_bwd > last_slash))
        last_slash = last_bwd;
#else
    const char *last_slash = last_fwd;
#endif
    if (!last_slash)
        return xr_strdup(".");

    /* Handle root "/" or "X:\" */
    if (last_slash == path)
        return xr_strdup("/");

    size_t len = (size_t) (last_slash - path);
    char *dir = (char *) xr_malloc(len + 1);
    if (!dir)
        return NULL;

    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

char *xr_path_join(const char *dir, const char *name) {
    if (!dir || !name)
        return NULL;

    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);

    /* Strip trailing slashes from dir */
    while (dir_len > 0 && dir[dir_len - 1] == '/') {
        dir_len--;
    }

    /* Handle empty dir after stripping */
    if (dir_len == 0)
        return xr_strdup(name);

    char *result = (char *) xr_malloc(dir_len + 1 + name_len + 1);
    if (!result)
        return NULL;

    memcpy(result, dir, dir_len);
    result[dir_len] = '/';
    memcpy(result + dir_len + 1, name, name_len);
    result[dir_len + 1 + name_len] = '\0';
    return result;
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

char *xr_realpath(const char *path, XrPathStatus *status) {
    XrPathStatus ignored;
    if (!status) status = &ignored;
    *status = XR_PATH_INVALID;
    if (!path)
        return NULL;

#ifdef XR_OS_WINDOWS
    XrWinPathStatus converted;
    wchar_t *wide = xr_win_utf8_path(path, &converted);
    if (!wide) {
        *status = fileio_conversion_status(converted);
        return NULL;
    }
    DWORD units = GetFullPathNameW(wide, 0, NULL, NULL);
    if (!units || units > 32768) {
        *status = units ? XR_PATH_BUDGET : fileio_windows_status(GetLastError());
        xr_free(wide); return NULL;
    }
    wchar_t *resolved = xr_malloc((size_t)units * sizeof(wchar_t));
    if (!resolved) { *status = XR_PATH_OUT_OF_MEMORY; xr_free(wide); return NULL; }
    DWORD length = GetFullPathNameW(wide, units, resolved, NULL);
    DWORD error = length ? ERROR_SUCCESS : GetLastError();
    xr_free(wide);
    char *result = NULL;
    if (length && length < units) {
        result = xr_win_utf16_text(resolved, &converted);
        *status = result ? XR_PATH_OK : fileio_conversion_status(converted);
    } else {
        *status = length ? XR_PATH_BUDGET : fileio_windows_status(error);
    }
    xr_free(resolved);
    return result;
#else
    char *rp = realpath(path, NULL);
    if (!rp) {
        *status = fileio_errno_status(errno);
        return NULL;
    }

    char *dup = xr_strdup(rp);
    *status = dup ? XR_PATH_OK : XR_PATH_OUT_OF_MEMORY;
    free(rp); /* xr:allow-raw-alloc realpath uses system malloc */
    return dup;
#endif
}
