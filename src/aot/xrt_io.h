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

#include "../shared/xr_cstr_core.h"
#include "../shared/xr_io_core.h"
#include "../shared/xr_os_core.h"
#include "../shared/xr_path_limit.h"
#include "xrt_arc.h"
#include "xrt_coll.h"
#include "xrt_value.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(XR_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <fcntl.h>
#if defined(_MSC_VER)
#include <corecrt_io.h>
#else
#include <io.h>
#endif
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
#include <fcntl.h>
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

#define XRT_IO_MAX_READ_BYTES ((long) INT32_MAX)

/* ========== File descriptor handles ==========
 *
 * A handle is a file descriptor, not a FILE* cast to an integer, so a bogus
 * integer arriving from Xray source answers EBADF instead of dereferencing an
 * arbitrary address. 0, 1 and 2 are reserved for the standard streams: reads
 * on 0 and writes on 1 and 2 go through the C runtime's own stdin/stdout/
 * stderr, which keeps them ordered against print(). This mirrors the VM half
 * in stdlib/io/io.c byte for byte -- the two backends must agree here.
 */
#define XRT_IO_STD_IN 0
#define XRT_IO_STD_OUT 1
#define XRT_IO_STD_ERR 2

#if defined(XR_OS_WINDOWS)
typedef int xrt_io_fd_ssize_t;
#define xrt_io_fd_open _open
#define xrt_io_fd_read _read
#define xrt_io_fd_write _write
#define xrt_io_fd_close _close
#define XRT_IO_FD_OPEN_FLAGS (_O_BINARY | _O_NOINHERIT)
#define XRT_IO_FD_CREATE_MODE (_S_IREAD | _S_IWRITE)
#else
typedef ssize_t xrt_io_fd_ssize_t;
#define xrt_io_fd_open open
#define xrt_io_fd_read read
#define xrt_io_fd_write write
#define xrt_io_fd_close close
#define XRT_IO_FD_OPEN_FLAGS O_CLOEXEC
#define XRT_IO_FD_CREATE_MODE 0644
#endif

// An open must never hand back 0, 1 or 2: a file landing on one of those would
// make a later write to "stdout" reach that file instead. Only reachable when
// the process starts with a standard stream already closed, which is why a
// failed lift refuses the open rather than returning the low descriptor.
static inline int xrt_io_fd_reserve_above_std(int fd) {
    if (fd < 0 || fd > XRT_IO_STD_ERR)
        return fd;
#if defined(XR_OS_WINDOWS)
    int lifted = _dup(fd);
    if (lifted >= 0 && lifted <= XRT_IO_STD_ERR) {
        _close(lifted);
        lifted = -1;
    }
#else
    int lifted = fcntl(fd, F_DUPFD_CLOEXEC, XRT_IO_STD_ERR + 1);
#endif
    xrt_io_fd_close(fd);
    return lifted;
}

// A non-bool tag fails closed to false rather than reading its payload.
static inline bool xrt_io_bool_arg(XrValue v) {
    return v.tag == XR_TAG_BOOL && v.i != 0;
}

static inline bool xrt_io_prepare_binary_stdin(void) {
#if defined(XR_OS_WINDOWS)
    return _setmode(_fileno(stdin), _O_BINARY) != -1;
#else
    return true;
#endif
}

static inline void *xrt_io_core_alloc(void *ctx, size_t size) {
    (void) ctx;
    return XRT_MALLOC(size);
}

static inline char *xrt_io_copy_cstr_arg(const char *data, int64_t len, char *stack,
                                         size_t stack_cap, char **owned) {
    return xr_cstr_core_copy_arg(data, len, stack, stack_cap, xrt_io_core_alloc, NULL, owned);
}

static inline XrValue xrt_io_remove(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    bool ok = path && remove(path) == 0;
    XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_io_rmdir(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    bool ok = false;
    if (path) {
#if defined(XR_OS_WINDOWS)
        ok = RemoveDirectoryA(path) != 0;
#else
        ok = rmdir(path) == 0;
#endif
    }
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

static inline XrValue xrt_io_cwd(void) {
    char buf[XR_PATH_LIMIT_MAX_PATH];
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
    XrValue obj =
        xrt_struct_object_new_named(XR_IO_CORE_STAT_FIELD_COUNT, XR_IO_CORE_STAT_FIELD_NAMES);
    xrt_object_set_field(obj, XR_IO_CORE_STAT_SIZE, XR_FROM_INT(fields.size));
    xrt_object_set_field(obj, XR_IO_CORE_STAT_MODE, XR_FROM_INT(fields.mode));
    xrt_object_set_field(obj, XR_IO_CORE_STAT_MTIME, XR_FROM_INT(fields.mtime));
    xrt_object_set_field(obj, XR_IO_CORE_STAT_ATIME, XR_FROM_INT(fields.atime));
    xrt_object_set_field(obj, XR_IO_CORE_STAT_CTIME, XR_FROM_INT(fields.ctime));
    xrt_object_set_field(obj, XR_IO_CORE_STAT_UID, XR_FROM_INT(fields.uid));
    xrt_object_set_field(obj, XR_IO_CORE_STAT_GID, XR_FROM_INT(fields.gid));
    xrt_object_set_field(obj, XR_IO_CORE_STAT_IS_FILE, XR_FROM_BOOL(fields.is_file));
    xrt_object_set_field(obj, XR_IO_CORE_STAT_IS_DIR, XR_FROM_BOOL(fields.is_dir));
    xrt_object_set_field(obj, XR_IO_CORE_STAT_IS_SYMLINK, XR_FROM_BOOL(fields.is_symlink));
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

static inline XrValue xrt_io_utime_now(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    bool ok = false;
    if (path) {
#if defined(XR_OS_WINDOWS)
        ok = _utime(path, NULL) == 0;
#else
        ok = utime(path, NULL) == 0;
#endif
    }
    XRT_FREE(owned);
    return XR_FROM_BOOL(ok);
}

static inline XrValue xrt_io_realpath(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    char resolved[XR_PATH_LIMIT_MAX_PATH];
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
               ? xrt_str_from_slice(view.data, view.len)
               : XR_NULL_VAL;
}

#if defined(XR_OS_WINDOWS)
static inline bool xrt_io_dir_for_each_entry(void *ctx, const char *path, XrIoCoreDirEntryFn visit,
                                             void *visit_ctx) {
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
        return false;

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

#else
static inline bool xrt_io_dir_for_each_entry(void *ctx, const char *path, XrIoCoreDirEntryFn visit,
                                             void *visit_ctx) {
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

#endif

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
    char buf[XR_PATH_LIMIT_MAX_PATH];
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
                    result = xrt_str_from_slice(view.data, view.len);
            }
        }
#else
        ssize_t len = readlink(path, buf, sizeof(buf) - 1);
        if (len >= 0) {
            XrIoCorePathView view;
            if (xr_io_core_path_result_view(buf, (size_t) len, &view))
                result = xrt_str_from_slice(view.data, view.len);
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

/* Create a uniquely named entry inside a caller-chosen root; the root and the
 * join are the module's decisions and reach here from its Xray body, so both
 * platforms are handed the same root. */
static inline bool xrt_io_temp_template(const char *root, char *out, size_t cap) {
    if (!root || root[0] == '\0')
        return false;
    int written = snprintf(out, cap, "%s/xray_XXXXXX", root);
    return written > 0 && (size_t) written < cap;
}

static inline XrValue xrt_io_make_temp_file(const char *root_data, int64_t root_len) {
    char stack_root[512];
    char *owned = NULL;
    char *root = xrt_io_copy_cstr_arg(root_data, root_len, stack_root, sizeof(stack_root), &owned);
    char tpl[XR_PATH_LIMIT_MAX_PATH];
    bool ok = root && xrt_io_temp_template(root, tpl, sizeof(tpl));
    if (!ok) {
        XRT_FREE(owned);
        return XR_NULL_VAL;
    }
#if defined(XR_OS_WINDOWS)
    char name[XR_PATH_LIMIT_MAX_PATH];
    ok = GetTempFileNameA(root, "xr_", 0, name) != 0;
    XRT_FREE(owned);
    return ok ? xrt_str_from_cstr(name) : XR_NULL_VAL;
#else
    XRT_FREE(owned);
    int fd = mkstemp(tpl);
    if (fd < 0)
        return XR_NULL_VAL;
    close(fd);
    return xrt_str_from_cstr(tpl);
#endif
}

static inline XrValue xrt_io_make_temp_dir(const char *root_data, int64_t root_len) {
    char stack_root[512];
    char *owned = NULL;
    char *root = xrt_io_copy_cstr_arg(root_data, root_len, stack_root, sizeof(stack_root), &owned);
    char tpl[XR_PATH_LIMIT_MAX_PATH];
    bool ok = root && xrt_io_temp_template(root, tpl, sizeof(tpl));
    if (!ok) {
        XRT_FREE(owned);
        return XR_NULL_VAL;
    }
#if defined(XR_OS_WINDOWS)
    char name[XR_PATH_LIMIT_MAX_PATH];
    ok = GetTempFileNameA(root, "xr_", 0, name) != 0;
    XRT_FREE(owned);
    if (!ok)
        return XR_NULL_VAL;
    DeleteFileA(name);
    if (!CreateDirectoryA(name, NULL))
        return XR_NULL_VAL;
    return xrt_str_from_cstr(name);
#else
    XRT_FREE(owned);
    if (!mkdtemp(tpl))
        return XR_NULL_VAL;
    return xrt_str_from_cstr(tpl);
#endif
}

typedef struct XrtIoReadDirEmitCtx {
    XrValue arr;
} XrtIoReadDirEmitCtx;

static inline bool xrt_io_read_dir_emit(void *ctx, const char *path) {
    XrtIoReadDirEmitCtx *emit = (XrtIoReadDirEmitCtx *) ctx;
    xrt_array_push(emit->arr, xrt_str_from_cstr(path));
    return true;
}

static inline XrValue xrt_io_read_dir(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    if (!path) {
        XRT_FREE(owned);
        return XR_NULL_VAL;
    }
    XrValue arr = xrt_array_new(0);
    XrtIoReadDirEmitCtx emit = {.arr = arr};
    if (!xrt_io_dir_for_each_entry(NULL, path, xrt_io_read_dir_emit, &emit)) {
        xrt_release(arr);
        XRT_FREE(owned);
        return XR_NULL_VAL;
    }
    XRT_FREE(owned);
    return arr;
}

static inline XrValue xrt_io_stream_read_bytes(FILE *stream, int64_t max_bytes) {
    if (!stream || max_bytes < 0 || max_bytes > INT32_MAX)
        return XR_NULL_VAL;
    XrValue out = xrt_array_new_typed_uninit(max_bytes, XR_ELEM_U8);
    if (!XR_IS_ARRAY(out) || !out.ptr)
        return XR_NULL_VAL;
    xrt_array_t *arr = (xrt_array_t *) out.ptr;
    if (max_bytes == 0)
        return out;
    size_t count = fread(arr->data, 1, (size_t) max_bytes, stream);
    if (count == 0 && ferror(stream)) {
        xrt_release(out);
        return XR_NULL_VAL;
    }
    arr->length = (int64_t) count;
    return out;
}

static inline int XR_IO_CORE_ACQUIRE_HANDLE("xray_file_descriptor")
    xrt_io_file_open_handle(const char *path) {
    if (!path || path[0] == '\0')
        return -1;
    return xrt_io_fd_reserve_above_std(xrt_io_fd_open(path, O_RDONLY | XRT_IO_FD_OPEN_FLAGS));
}

static inline int XR_IO_CORE_ACQUIRE_HANDLE("xray_file_descriptor")
    xrt_io_file_open_write_handle(const char *path, bool append) {
    if (!path || path[0] == '\0')
        return -1;
    int flags = O_WRONLY | O_CREAT | XRT_IO_FD_OPEN_FLAGS | (append ? O_APPEND : O_TRUNC);
    return xrt_io_fd_reserve_above_std(xrt_io_fd_open(path, flags, XRT_IO_FD_CREATE_MODE));
}

static inline bool
xrt_io_file_close_handle(int handle XR_IO_CORE_RELEASE_HANDLE("xray_file_descriptor")) {
    return xrt_io_fd_close(handle) == 0;
}

static inline XrValue xrt_io_file_open(const char *path_data, int64_t path_len) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    int handle = xrt_io_file_open_handle(path);
    XRT_FREE(owned);
    return XR_FROM_INT((int64_t) handle);
}

static inline XrValue xrt_io_file_open_write(const char *path_data, int64_t path_len,
                                             XrValue append) {
    char stack_path[512];
    char *owned = NULL;
    char *path = xrt_io_copy_cstr_arg(path_data, path_len, stack_path, sizeof(stack_path), &owned);
    int handle = xrt_io_file_open_write_handle(path, xrt_io_bool_arg(append));
    XRT_FREE(owned);
    return XR_FROM_INT((int64_t) handle);
}

static inline XrValue xrt_io_fd_read_bytes(int fd, int64_t max_bytes) {
    XrValue out = xrt_array_new_typed_uninit(max_bytes, XR_ELEM_U8);
    if (!XR_IS_ARRAY(out) || !out.ptr)
        return XR_NULL_VAL;
    xrt_array_t *arr = (xrt_array_t *) out.ptr;
    if (max_bytes == 0)
        return out;
    xrt_io_fd_ssize_t count;
    do {
        count = xrt_io_fd_read(fd, arr->data, (size_t) max_bytes);
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
        xrt_release(out);
        return XR_NULL_VAL;
    }
    arr->length = (int64_t) count;
    return out;
}

static inline XrValue xrt_io_read_handle_chunk(XrValue handle_value, XrValue max_bytes_value) {
    if (!XR_IS_INT(handle_value) || !XR_IS_INT(max_bytes_value))
        return XR_NULL_VAL;
    int64_t handle = XR_TO_INT(handle_value);
    int64_t max_bytes = XR_TO_INT(max_bytes_value);
    if (handle < 0 || max_bytes < 0 || max_bytes > XRT_IO_MAX_READ_BYTES)
        return XR_NULL_VAL;
    if (handle == XRT_IO_STD_IN) {
        if (!xrt_io_prepare_binary_stdin())
            return XR_NULL_VAL;
        return xrt_io_stream_read_bytes(stdin, max_bytes);
    }
    return xrt_io_fd_read_bytes((int) handle, max_bytes);
}

static inline XrValue xrt_io_file_close(XrValue handle_value) {
    if (!XR_IS_INT(handle_value))
        return XR_FROM_BOOL(false);
    int64_t handle = XR_TO_INT(handle_value);
    if (handle <= XRT_IO_STD_ERR)
        return XR_FROM_BOOL(false);  // the standard streams are not the caller's to close
    return XR_FROM_BOOL(xrt_io_file_close_handle((int) handle));
}

// Only the standard streams carry C-runtime buffering; a plain descriptor has
// none, so flushing one is a no-op that still reports success.
static inline XrValue xrt_io_file_flush(XrValue handle_value) {
    if (!XR_IS_INT(handle_value))
        return XR_FROM_BOOL(false);
    int64_t handle = XR_TO_INT(handle_value);
    if (handle < 0)
        return XR_FROM_BOOL(false);
    if (handle == XRT_IO_STD_OUT)
        return XR_FROM_BOOL(fflush(stdout) == 0);
    if (handle == XRT_IO_STD_ERR)
        return XR_FROM_BOOL(fflush(stderr) == 0);
    return XR_FROM_BOOL(true);
}

/* One write, reported as the number of bytes the stream accepted. `offset` is
 * an index into the caller's buffer, never a file offset: the file position is
 * the descriptor's own, so an appending handle keeps appending. The retry loop
 * that turns a short write into a complete one lives in io.xr. */
static inline XrValue xrt_io_write_once(int64_t handle, const char *data, size_t len,
                                        int64_t offset) {
    if (handle < 0 || offset < 0 || (!data && len != 0))
        return XR_FROM_INT(-1);
    if ((uint64_t) offset >= (uint64_t) len)
        return XR_FROM_INT(0);
    const char *from = data + offset;
    size_t remaining = len - (size_t) offset;

    if (handle == XRT_IO_STD_OUT || handle == XRT_IO_STD_ERR) {
        FILE *stream = (handle == XRT_IO_STD_OUT) ? stdout : stderr;
        size_t n = fwrite(from, 1, remaining, stream);
        if (n == 0 && ferror(stream))
            return XR_FROM_INT(-1);
        return XR_FROM_INT((int64_t) n);
    }
    if (handle == XRT_IO_STD_IN)
        return XR_FROM_INT(-1);  // stdin is not writable

    xrt_io_fd_ssize_t n;
    do {
        n = xrt_io_fd_write((int) handle, from, remaining);
    } while (n < 0 && errno == EINTR);
    return XR_FROM_INT(n < 0 ? -1 : (int64_t) n);
}

static inline XrValue xrt_io_write_handle_chunk(XrValue handle_value, XrValue bytes,
                                                XrValue offset_value) {
    if (!XR_IS_INT(handle_value) || !XR_IS_INT(offset_value))
        return XR_FROM_INT(-1);
    if (!XR_IS_ARRAY(bytes) || !bytes.ptr)
        return XR_FROM_INT(-1);
    xrt_array_t *arr = (xrt_array_t *) bytes.ptr;
    if (arr->elem_type != XR_ELEM_U8)
        return XR_FROM_INT(-1);
    return xrt_io_write_once(XR_TO_INT(handle_value), (const char *) arr->data,
                             (size_t) arr->length, XR_TO_INT(offset_value));
}

static inline XrValue xrt_io_write_handle_str(XrValue handle_value, const char *data, int64_t len,
                                              XrValue offset_value) {
    if (!XR_IS_INT(handle_value) || !XR_IS_INT(offset_value) || len < 0)
        return XR_FROM_INT(-1);
    return xrt_io_write_once(XR_TO_INT(handle_value), data ? data : "", (size_t) len,
                             XR_TO_INT(offset_value));
}

#endif  // XRT_IO_H
