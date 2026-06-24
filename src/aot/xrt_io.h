/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_io.h - Freestanding AOT helpers for synchronous filesystem I/O.
 */

#ifndef XRT_IO_H
#define XRT_IO_H

#include "../shared/xr_io_core.h"
#include "../shared/xr_os_core.h"
#include "xrt_arc.h"
#include "xrt_coll.h"
#include "xrt_value.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(XR_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/utime.h>
#include <windows.h>
#define xrt_io_platform_getcwd _getcwd
#define xrt_io_platform_chdir _chdir
#define xrt_io_platform_mkdir(path) _mkdir(path)
#define xrt_io_platform_chmod _chmod
#define xrt_io_platform_utime _utime
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>
#define xrt_io_platform_getcwd getcwd
#define xrt_io_platform_chdir chdir
#define xrt_io_platform_mkdir(path) mkdir((path), 0755)
#define xrt_io_platform_chmod chmod
#define xrt_io_platform_utime utime
#endif

#ifndef XRT_IO_PATH_MAX
#if defined(XR_OS_WINDOWS)
#define XRT_IO_PATH_MAX 4096
#elif defined(PATH_MAX)
#define XRT_IO_PATH_MAX PATH_MAX
#else
#define XRT_IO_PATH_MAX 4096
#endif
#endif

#define XRT_IO_MAX_READ_BYTES ((long) INT32_MAX)

static inline void *xrt_io_core_alloc(void *ctx, size_t size) {
    (void) ctx;
    return XRT_MALLOC(size);
}

static inline void *xrt_io_core_realloc(void *ctx, void *ptr, size_t size) {
    (void) ctx;
    return XRT_REALLOC(ptr, size);
}

static inline void xrt_io_core_free(void *ctx, void *ptr) {
    (void) ctx;
    XRT_FREE(ptr);
}

static inline bool xrt_io_file_seek_end(void *ctx) {
    return fseek((FILE *) ctx, 0, SEEK_END) == 0;
}

static inline long xrt_io_file_tell(void *ctx) {
    return ftell((FILE *) ctx);
}

static inline bool xrt_io_file_seek_start(void *ctx) {
    return fseek((FILE *) ctx, 0, SEEK_SET) == 0;
}

static inline size_t xrt_io_file_read(void *ctx, void *buf, size_t cap) {
    return fread(buf, 1, cap, (FILE *) ctx);
}

static inline size_t xrt_io_file_write(void *ctx, const void *buf, size_t len) {
    return fwrite(buf, 1, len, (FILE *) ctx);
}

static inline bool xrt_io_file_error(void *ctx) {
    return ferror((FILE *) ctx) != 0;
}

static inline char *xrt_io_copy_cstr_arg(const char *data, int64_t len, char *stack,
                                         size_t stack_cap, char **owned) {
    if (owned)
        *owned = NULL;
    if (!data || len < 0)
        return NULL;
    size_t n = (size_t) len;
    char *out = stack;
    if (n + 1 > stack_cap) {
        out = (char *) XRT_MALLOC(n + 1);
        if (!out)
            return NULL;
        if (owned)
            *owned = out;
    }
    memcpy(out, data, n);
    out[n] = '\0';
    return out;
}

static inline XrValue xrt_io_str_slice(const char *data, size_t len) {
    XrValue out = xrt_str_alloc(len);
    if (len > 0 && data)
        memcpy(xr_str_buf(out), data, len);
    return out;
}

static inline int xrt_io_stat_path(const char *path, struct stat *st) {
    if (!path || !st)
        return -1;
    return stat(path, st);
}

static inline XrValue xrt_io_exists(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    struct stat st;
    bool ok = path && stat(path, &st) == 0;
    XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_io_is_file(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    struct stat st;
    bool ok = false;
    if (path && stat(path, &st) == 0) {
#if defined(XR_OS_WINDOWS)
        ok = (st.st_mode & _S_IFREG) != 0;
#else
        ok = S_ISREG(st.st_mode);
#endif
    }
    XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_io_is_dir(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    struct stat st;
    bool ok = false;
    if (path && stat(path, &st) == 0) {
#if defined(XR_OS_WINDOWS)
        ok = (st.st_mode & _S_IFDIR) != 0;
#else
        ok = S_ISDIR(st.st_mode);
#endif
    }
    XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_io_file_size(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    struct stat st;
    int64_t size = -1;
    if (path && stat(path, &st) == 0)
        size = (int64_t) st.st_size;
    XRT_FREE(owned);
    return XR_FROM_INT(size);
}

static inline char *xrt_io_read_file_buffer(const char *path, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!path || path[0] == '\0')
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = xr_io_core_read_sized_stream_alloc(
        f, xrt_io_file_seek_end, xrt_io_file_tell, xrt_io_file_seek_start, xrt_io_file_read,
        xrt_io_file_error, xrt_io_core_alloc, xrt_io_core_free, NULL, XRT_IO_MAX_READ_BYTES,
        out_len);
    fclose(f);
    return buf;
}

static inline XrValue xrt_io_read_file(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    size_t len = 0;
    char *buf = xrt_io_read_file_buffer(path, &len);
    XRT_FREE(owned);
    if (!buf)
        return XR_NULL_VAL;
    XrValue out = xrt_io_str_slice(buf, len);
    XRT_FREE(buf);
    return out;
}

static inline XrValue xrt_io_read_file_bytes(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    size_t len = 0;
    char *buf = xrt_io_read_file_buffer(path, &len);
    XRT_FREE(owned);
    if (!buf)
        return XR_NULL_VAL;
    XrValue out = xrt_array_new_typed_uninit((int64_t) len, XR_ELEM_U8);
    xrt_array_t *arr = (xrt_array_t *) out.ptr;
    if (len > 0)
        memcpy(arr->data, buf, len);
    arr->length = (int64_t) len;
    XRT_FREE(buf);
    return out;
}

static inline XrValue xrt_io_write_buffer(const char *path, const char *data, size_t len,
                                          const char *mode) {
    if (!path || !data || !mode)
        return XR_FROM_BOOL(false);
    FILE *f = fopen(path, mode);
    if (!f)
        return XR_FROM_BOOL(false);
    bool ok = xr_io_core_write_all(f, xrt_io_file_write, xrt_io_file_error, data, len);
    bool close_ok = fclose(f) == 0;
    ok = ok && close_ok;
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_io_write_file(const char *path_data, int64_t path_len, const char *data,
                                        int64_t data_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    XrValue ok =
        xrt_io_write_buffer(path, data ? data : "", data_len < 0 ? 0 : (size_t) data_len, "wb");
    XRT_FREE(owned);
    return ok;
}

static inline XrValue xrt_io_append_file(const char *path_data, int64_t path_len, const char *data,
                                         int64_t data_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    XrValue ok =
        xrt_io_write_buffer(path, data ? data : "", data_len < 0 ? 0 : (size_t) data_len, "ab");
    XRT_FREE(owned);
    return ok;
}

static inline XrValue xrt_io_write_file_bytes(const char *path_data, int64_t path_len,
                                              XrValue bytes) {
    if (!XR_IS_ARRAY(bytes) || !bytes.ptr)
        return XR_FROM_BOOL(false);
    xrt_array_t *arr = (xrt_array_t *) bytes.ptr;
    if (arr->elem_type != XR_ELEM_U8)
        return XR_FROM_BOOL(false);
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    XrValue ok = xrt_io_write_buffer(path, (const char *) arr->data, (size_t) arr->length, "wb");
    XRT_FREE(owned);
    return ok;
}

static inline XrValue xrt_io_remove(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    bool ok = path && remove(path) == 0;
    XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_io_rename(const char *old_data, int64_t old_len, const char *new_data,
                                    int64_t new_len) {
    char old_stack[512];
    char new_stack[512];
    char *old_owned = NULL;
    char *new_owned = NULL;
    char *old_path =
        xrt_io_copy_cstr_arg(old_data, old_len, old_stack, sizeof(old_stack), &old_owned);
    char *new_path =
        xrt_io_copy_cstr_arg(new_data, new_len, new_stack, sizeof(new_stack), &new_owned);
    bool ok = old_path && new_path && rename(old_path, new_path) == 0;
    XRT_FREE(old_owned);
    XRT_FREE(new_owned);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_io_mkdir(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    bool ok = path && xrt_io_platform_mkdir(path) == 0;
    if (!ok && path) {
        struct stat st;
        ok = stat(path, &st) == 0 &&
#if defined(XR_OS_WINDOWS)
             ((st.st_mode & _S_IFDIR) != 0);
#else
             S_ISDIR(st.st_mode);
#endif
    }
    XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline int xrt_io_mkdirp_mkdir(void *ctx, const char *path) {
    (void) ctx;
    return xrt_io_platform_mkdir(path);
}

static inline bool xrt_io_mkdirp_is_dir(void *ctx, const char *path) {
    (void) ctx;
    struct stat st;
    if (!path || stat(path, &st) != 0)
        return false;
#if defined(XR_OS_WINDOWS)
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

static inline XrValue xrt_io_mkdirp(const char *path_data, int64_t path_len) {
    char stack_path[XRT_IO_PATH_MAX];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    bool ok = path && xr_io_core_mkdirp(path, xrt_io_mkdirp_mkdir, xrt_io_mkdirp_is_dir, NULL);
    XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_io_cwd(void) {
    char buf[XRT_IO_PATH_MAX];
    if (!xrt_io_platform_getcwd(buf, sizeof(buf)))
        return XR_NULL_VAL;
    return xrt_str_from_cstr(buf);
}

static inline XrValue xrt_io_chdir(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    bool ok = path && xrt_io_platform_chdir(path) == 0;
    XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

typedef struct XrtIoCopyFileCtx {
    FILE *src;
    FILE *dst;
} XrtIoCopyFileCtx;

static inline size_t xrt_io_copy_file_read(void *ctx, void *buf, size_t cap) {
    XrtIoCopyFileCtx *copy_ctx = (XrtIoCopyFileCtx *) ctx;
    return fread(buf, 1, cap, copy_ctx->src);
}

static inline size_t xrt_io_copy_file_write(void *ctx, const void *buf, size_t len) {
    XrtIoCopyFileCtx *copy_ctx = (XrtIoCopyFileCtx *) ctx;
    return fwrite(buf, 1, len, copy_ctx->dst);
}

static inline bool xrt_io_copy_file_error(void *ctx) {
    XrtIoCopyFileCtx *copy_ctx = (XrtIoCopyFileCtx *) ctx;
    return ferror(copy_ctx->src) != 0;
}

static inline XrValue xrt_io_copy_file(const char *src_data, int64_t src_len, const char *dst_data,
                                       int64_t dst_len) {
    char src_stack[512];
    char dst_stack[512];
    char *src_owned = NULL;
    char *dst_owned = NULL;
    char *src = xrt_io_copy_cstr_arg(src_data, src_len, src_stack, sizeof(src_stack), &src_owned);
    char *dst = xrt_io_copy_cstr_arg(dst_data, dst_len, dst_stack, sizeof(dst_stack), &dst_owned);
    bool ok = false;
    if (src && dst) {
        FILE *in = fopen(src, "rb");
        if (in) {
            FILE *out = fopen(dst, "wb");
            if (out) {
                char buf[XR_IO_CORE_COPY_BUFFER_SIZE];
                XrtIoCopyFileCtx copy_ctx = {.src = in, .dst = out};
                ok =
                    xr_io_core_copy_stream(&copy_ctx, xrt_io_copy_file_read, xrt_io_copy_file_write,
                                           xrt_io_copy_file_error, buf, sizeof(buf));
                if (fclose(out) != 0)
                    ok = false;
            }
            fclose(in);
        }
    }
    XRT_FREE(src_owned);
    XRT_FREE(dst_owned);
    return XR_FROM_BOOL(ok);
}

typedef struct XrtIoReadLinesCtx {
    XrValue arr;
} XrtIoReadLinesCtx;

static inline bool xrt_io_read_lines_push(void *ctx, const char *data, size_t len) {
    XrtIoReadLinesCtx *read_ctx = (XrtIoReadLinesCtx *) ctx;
    xrt_array_push(read_ctx->arr, xrt_io_str_slice(data, len));
    return true;
}

static inline XrValue xrt_io_read_lines(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    size_t len = 0;
    char *buf = xrt_io_read_file_buffer(path, &len);
    XRT_FREE(owned);
    if (!buf)
        return XR_NULL_VAL;
    XrValue arr = xrt_array_new(8);
    XrtIoReadLinesCtx read_ctx = {arr};
    xr_io_core_read_lines_each(buf, len, xrt_io_read_lines_push, &read_ctx);
    XRT_FREE(buf);
    return arr;
}

static inline XrValue xrt_io_is_symlink(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    bool ok = false;
#if defined(XR_OS_WINDOWS)
    DWORD attrs = path ? GetFileAttributesA(path) : INVALID_FILE_ATTRIBUTES;
    ok = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct stat st;
    ok = path && lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
#endif
    XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_io_stat(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    struct stat st;
    if (!path || stat(path, &st) != 0) {
        XRT_FREE(owned);
        return XR_NULL_VAL;
    }
#if defined(XR_OS_WINDOWS)
    DWORD attrs = GetFileAttributesA(path);
    bool is_symlink = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
    XrIoCoreStatFields fields = xr_io_core_stat_fields(
        (int64_t) st.st_size, (int64_t) st.st_mode, (int64_t) st.st_mtime, (int64_t) st.st_atime,
        (int64_t) st.st_ctime, 0, 0, (st.st_mode & _S_IFREG) != 0, (st.st_mode & _S_IFDIR) != 0,
        is_symlink);
#else
    struct stat lst;
    bool is_symlink = lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode);
    XrIoCoreStatFields fields = xr_io_core_stat_fields(
        (int64_t) st.st_size, (int64_t) st.st_mode, (int64_t) st.st_mtime, (int64_t) st.st_atime,
        (int64_t) st.st_ctime, (int64_t) st.st_uid, (int64_t) st.st_gid, S_ISREG(st.st_mode),
        S_ISDIR(st.st_mode), is_symlink);
#endif
    XrValue obj = xrt_json_new_named(XR_IO_CORE_STAT_FIELD_COUNT, XR_IO_CORE_STAT_FIELD_NAMES);
    xrt_json_set_field(obj, XR_IO_CORE_STAT_SIZE, XR_FROM_INT(fields.size));
    xrt_json_set_field(obj, XR_IO_CORE_STAT_MODE, XR_FROM_INT(fields.mode));
    xrt_json_set_field(obj, XR_IO_CORE_STAT_MTIME, XR_FROM_INT(fields.mtime));
    xrt_json_set_field(obj, XR_IO_CORE_STAT_ATIME, XR_FROM_INT(fields.atime));
    xrt_json_set_field(obj, XR_IO_CORE_STAT_CTIME, XR_FROM_INT(fields.ctime));
    xrt_json_set_field(obj, XR_IO_CORE_STAT_UID, XR_FROM_INT(fields.uid));
    xrt_json_set_field(obj, XR_IO_CORE_STAT_GID, XR_FROM_INT(fields.gid));
    xrt_json_set_field(obj, XR_IO_CORE_STAT_IS_FILE, XR_FROM_BOOL(fields.is_file));
    xrt_json_set_field(obj, XR_IO_CORE_STAT_IS_DIR, XR_FROM_BOOL(fields.is_dir));
    xrt_json_set_field(obj, XR_IO_CORE_STAT_IS_SYMLINK, XR_FROM_BOOL(fields.is_symlink));
    XRT_FREE(owned);
    return obj;
}

static inline XrValue xrt_io_chmod_value(const char *path_data, int64_t path_len,
                                         XrValue mode_value) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    int mode = 0;
    bool ok = path && XR_IS_INT(mode_value) &&
              xr_io_core_chmod_mode(XR_TO_INT(mode_value), &mode) &&
              xrt_io_platform_chmod(path, mode) == 0;
    XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline bool xrt_io_touch_update(void *ctx, const char *path) {
    (void) ctx;
    return xrt_io_platform_utime(path, NULL) == 0;
}

static inline bool xrt_io_touch_create(void *ctx, const char *path) {
    (void) ctx;
    FILE *f = fopen(path, "a");
    if (!f)
        return false;
    return fclose(f) == 0;
}

static inline XrValue xrt_io_touch(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    bool ok = xr_io_core_touch(path, xrt_io_touch_update, xrt_io_touch_create, NULL);
    XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_io_realpath(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    char resolved[XRT_IO_PATH_MAX];
    char *ok = NULL;
    if (path) {
#if defined(XR_OS_WINDOWS)
        ok = _fullpath(resolved, path, sizeof(resolved));
#else
        ok = realpath(path, resolved);
#endif
    }
    XRT_FREE(owned);
    XrIoCorePathView view;
    return ok && xr_io_core_path_result_cstr_view(resolved, &view)
               ? xrt_io_str_slice(view.data, view.len)
               : XR_NULL_VAL;
}

#if defined(XR_OS_WINDOWS)
static inline XrIoCorePathKind xrt_io_remove_all_kind(void *ctx, const char *path) {
    (void) ctx;
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return XR_IO_CORE_PATH_MISSING;
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT))
        return XR_IO_CORE_PATH_DIR;
    return XR_IO_CORE_PATH_LEAF;
}

static inline bool xrt_io_remove_all_for_each_entry(void *ctx, const char *path,
                                                    XrIoCoreDirEntryFn visit, void *visit_ctx) {
    (void) ctx;
    size_t len = strlen(path);
    char *pattern = (char *) XRT_MALLOC(len + 4);
    if (!pattern)
        return false;
    memcpy(pattern, path, len);
    pattern[len++] = '\\';
    pattern[len++] = '*';
    pattern[len] = '\0';

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    XRT_FREE(pattern);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;

    bool ok = true;
    do {
        if (!visit(visit_ctx, fd.cFileName)) {
            ok = false;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

static inline bool xrt_io_remove_all_leaf(void *ctx, const char *path) {
    (void) ctx;
    DWORD attrs = GetFileAttributesA(path);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return RemoveDirectoryA(path) != 0;
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    return DeleteFileA(path) != 0;
}

static inline bool xrt_io_remove_all_dir(void *ctx, const char *path) {
    (void) ctx;
    return RemoveDirectoryA(path) != 0;
}
#else
static inline XrIoCorePathKind xrt_io_remove_all_kind(void *ctx, const char *path) {
    (void) ctx;
    struct stat st;
    if (lstat(path, &st) != 0)
        return XR_IO_CORE_PATH_MISSING;
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
        return XR_IO_CORE_PATH_LEAF;
    return XR_IO_CORE_PATH_DIR;
}

static inline bool xrt_io_remove_all_for_each_entry(void *ctx, const char *path,
                                                    XrIoCoreDirEntryFn visit, void *visit_ctx) {
    (void) ctx;
    DIR *dir = opendir(path);
    if (!dir)
        return false;
    bool ok = true;
    for (;;) {
        struct dirent *e = readdir(dir);
        if (!e)
            break;
        if (!visit(visit_ctx, e->d_name)) {
            ok = false;
            break;
        }
    }
    closedir(dir);
    return ok;
}

static inline bool xrt_io_remove_all_leaf(void *ctx, const char *path) {
    (void) ctx;
    return remove(path) == 0;
}

static inline bool xrt_io_remove_all_dir(void *ctx, const char *path) {
    (void) ctx;
    return rmdir(path) == 0;
}
#endif

static inline XrValue xrt_io_remove_all(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    XrIoCoreRemoveAllOps ops = {
        .kind = xrt_io_remove_all_kind,
        .for_each_entry = xrt_io_remove_all_for_each_entry,
        .remove_leaf = xrt_io_remove_all_leaf,
        .remove_dir = xrt_io_remove_all_dir,
        .alloc = xrt_io_core_alloc,
        .free = xrt_io_core_free,
        .sep =
#if defined(XR_OS_WINDOWS)
            '\\',
#else
            '/',
#endif
    };
    bool ok = path && xr_io_core_remove_all(path, &ops, NULL);
    XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_io_symlink(const char *target_data, int64_t target_len,
                                     const char *path_data, int64_t path_len) {
    char target_stack[512];
    char path_stack[512];
    char *target_owned = NULL;
    char *path_owned = NULL;
    char *target = xrt_io_copy_cstr_arg(target_data, target_len, target_stack, sizeof(target_stack),
                                        &target_owned);
    char *path =
        xrt_io_copy_cstr_arg(path_data, path_len, path_stack, sizeof(path_stack), &path_owned);
    bool ok = false;
    if (target && path) {
#if defined(XR_OS_WINDOWS)
        DWORD flags = 0;
        DWORD attrs = GetFileAttributesA(target);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
        flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        ok = CreateSymbolicLinkA(path, target, flags) != 0;
#else
        ok = symlink(target, path) == 0;
#endif
    }
    XRT_FREE(target_owned);
    XRT_FREE(path_owned);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_io_readlink(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    char buf[XRT_IO_PATH_MAX];
    XrValue result = XR_NULL_VAL;
    if (path) {
#if defined(XR_OS_WINDOWS)
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD len = GetFinalPathNameByHandleA(h, buf, sizeof(buf), FILE_NAME_NORMALIZED);
            CloseHandle(h);
            if (len > 0 && len < sizeof(buf)) {
                XrIoCorePathView view;
                if (xr_io_core_path_result_view(buf, (size_t) len, &view))
                    result = xrt_io_str_slice(view.data, view.len);
            }
        }
#else
        ssize_t len = readlink(path, buf, sizeof(buf) - 1);
        if (len >= 0) {
            XrIoCorePathView view;
            if (xr_io_core_path_result_view(buf, (size_t) len, &view))
                result = xrt_io_str_slice(view.data, view.len);
        }
#endif
    }
    XRT_FREE(owned);
    return result;
}

static inline const char *xrt_io_core_getenv(void *ctx, const char *name) {
    (void) ctx;
    return getenv(name);
}

static inline XrValue xrt_io_temp_file(void) {
    char tpl[XRT_IO_PATH_MAX];
#if defined(XR_OS_WINDOWS)
    char tmpdir[MAX_PATH];
    if (GetTempPathA(sizeof(tmpdir), tmpdir) == 0)
        return XR_NULL_VAL;
    char tmpfile[MAX_PATH];
    if (GetTempFileNameA(tmpdir, "xr_", 0, tmpfile) == 0)
        return XR_NULL_VAL;
    snprintf(tpl, sizeof(tpl), "%s", tmpfile);
#else
    const char *root = xr_os_core_tmpdir(xrt_io_core_getenv, NULL);
    if (!xr_io_core_temp_template(root, '/', "xray_XXXXXX", tpl, sizeof(tpl)))
        return XR_NULL_VAL;
    int fd = mkstemp(tpl);
    if (fd < 0)
        return XR_NULL_VAL;
    close(fd);
#endif
    return xrt_str_from_cstr(tpl);
}

static inline XrValue xrt_io_temp_dir(void) {
    char tpl[XRT_IO_PATH_MAX];
#if defined(XR_OS_WINDOWS)
    char tmpdir[MAX_PATH];
    if (GetTempPathA(sizeof(tmpdir), tmpdir) == 0)
        return XR_NULL_VAL;
    char tmpfile[MAX_PATH];
    if (GetTempFileNameA(tmpdir, "xr_", 0, tmpfile) == 0)
        return XR_NULL_VAL;
    DeleteFileA(tmpfile);
    if (!CreateDirectoryA(tmpfile, NULL))
        return XR_NULL_VAL;
    snprintf(tpl, sizeof(tpl), "%s", tmpfile);
#else
    const char *root = xr_os_core_tmpdir(xrt_io_core_getenv, NULL);
    if (!xr_io_core_temp_template(root, '/', "xray_XXXXXX", tpl, sizeof(tpl)))
        return XR_NULL_VAL;
    if (!mkdtemp(tpl))
        return XR_NULL_VAL;
#endif
    return xrt_str_from_cstr(tpl);
}

static inline XrValue xrt_io_read_dir(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    if (!path) {
        XRT_FREE(owned);
        return XR_NULL_VAL;
    }
    XrValue arr = xrt_array_new(8);
#if defined(XR_OS_WINDOWS)
    size_t plen = strlen(path);
    char *pattern = (char *) XRT_MALLOC(plen + 4);
    if (!pattern) {
        XRT_FREE(owned);
        return XR_NULL_VAL;
    }
    snprintf(pattern, plen + 4, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    XRT_FREE(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        XRT_FREE(owned);
        return XR_NULL_VAL;
    }
    do {
        const char *name = fd.cFileName;
        if (!xr_io_core_is_dot_dir_entry(name))
            xrt_array_push(arr, xrt_str_from_cstr(name));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dir = opendir(path);
    if (!dir) {
        XRT_FREE(owned);
        return XR_NULL_VAL;
    }
    for (;;) {
        struct dirent *e = readdir(dir);
        if (!e)
            break;
        const char *name = e->d_name;
        if (xr_io_core_is_dot_dir_entry(name))
            continue;
        xrt_array_push(arr, xrt_str_from_cstr(name));
    }
    closedir(dir);
#endif
    XRT_FREE(owned);
    return arr;
}

static inline void xrt_io_read_dir_recursive_impl(XrValue arr, const char *base, const char *path,
                                                  int depth) {
    if (!base || !path || depth >= 64)
        return;
    size_t base_len = strlen(base);
#if defined(XR_OS_WINDOWS)
    size_t plen = strlen(path);
    char *pattern = (char *) XRT_MALLOC(plen + 4);
    if (!pattern)
        return;
    snprintf(pattern, plen + 4, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    XRT_FREE(pattern);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const char *name = fd.cFileName;
        if (xr_io_core_is_dot_dir_entry(name))
            continue;
        size_t child_len = 0;
        if (!xr_io_core_join_child_len(path, name, &child_len))
            continue;
        char *child = (char *) XRT_MALLOC(child_len + 1);
        if (!child)
            continue;
        if (!xr_io_core_join_child_path(path, '\\', name, child, child_len + 1)) {
            XRT_FREE(child);
            continue;
        }
        const char *rel = xr_io_core_relative_path_from_base(child, base_len);
        xrt_array_push(arr, xrt_str_from_cstr(rel));
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            xrt_io_read_dir_recursive_impl(arr, base, child, depth + 1);
        XRT_FREE(child);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dir = opendir(path);
    if (!dir)
        return;
    for (;;) {
        struct dirent *e = readdir(dir);
        if (!e)
            break;
        const char *name = e->d_name;
        if (xr_io_core_is_dot_dir_entry(name))
            continue;
        size_t child_len = 0;
        if (!xr_io_core_join_child_len(path, name, &child_len))
            continue;
        char *child = (char *) XRT_MALLOC(child_len + 1);
        if (!child)
            continue;
        if (!xr_io_core_join_child_path(path, '/', name, child, child_len + 1)) {
            XRT_FREE(child);
            continue;
        }
        const char *rel = xr_io_core_relative_path_from_base(child, base_len);
        xrt_array_push(arr, xrt_str_from_cstr(rel));
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            xrt_io_read_dir_recursive_impl(arr, base, child, depth + 1);
        XRT_FREE(child);
    }
    closedir(dir);
#endif
}

static inline XrValue xrt_io_read_dir_recursive(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    if (!path) {
        XRT_FREE(owned);
        return XR_NULL_VAL;
    }
    XrValue arr = xrt_array_new(8);
    xrt_io_read_dir_recursive_impl(arr, path, path, 0);
    XRT_FREE(owned);
    return arr;
}

static inline XrValue xrt_io_read_stdin(void) {
    clearerr(stdin);
    size_t len = 0;
    char *buf = xr_io_core_read_all_stream_alloc(
        stdin, xrt_io_file_read, xrt_io_file_error, xrt_io_core_alloc, xrt_io_core_realloc,
        xrt_io_core_free, NULL, 4096, XRT_IO_MAX_READ_BYTES, &len);
    if (!buf)
        return XR_NULL_VAL;
    XrValue out = xrt_io_str_slice(buf, len);
    XRT_FREE(buf);
    return out;
}

#endif  // XRT_IO_H
