/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * io.c - File I/O standard library implementation
 *
 * KEY CONCEPT:
 *   Minimal POSIX, Win32, descriptor and stdio leaves for io.xr. Path policy,
 *   whole-input buffering, UTF-8 validation and public value construction do
 *   not live in this translation unit.
 */

#include "io.h"
#include "../common.h"
#include "../../src/io/xfile_provider.h"
#include "../../src/module/xstdlib_runtime_cache.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/class/xinstance.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/mem/xcoro_heap.h"
#include "../../src/runtime/mem/xalloc_unified.h"
#include "../../src/base/xmalloc.h"
#include "../../src/base/xchecks.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/coro/xnetpoll.h"
#include "../../src/coro/xworker.h"
#include "../../src/shared/xr_io_core.h"
#include "../../src/shared/xr_os_core.h"
#include "../../src/vm/xvm.h"  // xr_yieldable_cfunction_new (XRS_EXPORT_YIELDABLE)
#include "../../src/runtime/xisolate_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/os/os_fs.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>

#ifdef XR_OS_WINDOWS
#include <fcntl.h>
#if defined(_MSC_VER)
#include <corecrt_io.h>
#else
#include <io.h>
#endif
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utime.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <utime.h>
#endif

/* ========== External Declarations ========== */

struct XrCoroutine;
extern struct XrCoroutine *xr_current_coro(XrVMRuntime *X);

/* ========== File descriptor handles ==========
 *
 * A handle is a file descriptor, not a FILE* cast to an integer. The kernel
 * validates it, so a bogus integer arriving from Xray source answers EBADF
 * instead of dereferencing an arbitrary address, and a descriptor is what
 * io_uring accepts -- a FILE* has no submittable form.
 *
 * 0, 1 and 2 are reserved for the standard streams. Reads on 0 and writes on
 * 1 and 2 go through the C runtime's own stdin/stdout/stderr rather than the
 * raw descriptor, so they stay ordered against print(), which writes the same
 * FILE*. An open never hands back a descriptor in that range.
 */
#define XR_IO_STD_IN 0
#define XR_IO_STD_OUT 1
#define XR_IO_STD_ERR 2

/* ========== File Read/Write ========== */

static XrValue io_fileOpen(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(-1);
    const char *path = xrs_path_arg(args[0], NULL);
    return xr_int((int64_t) xr_file_open_read(path));
}

static XrValue io_fileOpenWrite(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_int(-1);
    const char *path = xrs_path_arg(args[0], NULL);
    return xr_int((int64_t) xr_file_open_write(path, XR_IS_TRUE(args[1])));
}

static XrValue io_fileRead(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_null();
    int64_t handle = XR_TO_INT(args[0]);
    int64_t max_bytes = XR_TO_INT(args[1]);
    if (handle < 0 || max_bytes < 0 || max_bytes > INT32_MAX)
        return xr_null();
    XrArray *arr = xr_byte_array_new(xr_current_coro(X), (int32_t) max_bytes);
    if (!arr)
        return xr_null();
    if (max_bytes == 0)
        return xr_value_from_array(arr);
    int64_t count = xr_file_read_once(handle, arr->data, (size_t) max_bytes);
    if (count < 0) {
        xr_rc_release_value(xr_current_coro_heap(), xr_value_from_array(arr));
        return xr_null();
    }
    arr->length = (int32_t) count;
    return xr_value_from_array(arr);
}

static XrValue io_fileClose(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_bool(false);
    int64_t handle = XR_TO_INT(args[0]);
    if (handle <= XR_IO_STD_ERR)
        return xr_bool(false);  // the standard streams are not the caller's to close
    return xr_bool(xr_file_close((int) handle));
}

// Only the standard streams carry C-runtime buffering; a plain descriptor has
// none, so flushing one is a no-op that still reports success.
static XrValue io_fileFlush(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_bool(false);
    int64_t handle = XR_TO_INT(args[0]);
    if (handle < 0)
        return xr_bool(false);
    if (handle == XR_IO_STD_OUT)
        return xr_bool(fflush(stdout) == 0);
    if (handle == XR_IO_STD_ERR)
        return xr_bool(fflush(stderr) == 0);
    return xr_bool(true);
}

static XrCFuncResult io_fileWrite(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    *result = xr_int(-1);
    if (argc < 3 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[2]))
        return XR_CFUNC_DONE;
    if (!xr_value_is_array(args[1]))
        return XR_CFUNC_DONE;
    XrArray *arr = xr_value_to_array(args[1]);
    if (!arr || arr->elem_type != XR_ELEM_U8)
        return XR_CFUNC_DONE;
    return xr_file_write_once(X, XR_TO_INT(args[0]), (const char *) arr->data, (size_t) arr->length,
                              XR_TO_INT(args[2]), result);
}

static XrCFuncResult io_fileWriteStr(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    *result = xr_int(-1);
    if (argc < 3 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[2]))
        return XR_CFUNC_DONE;
    size_t len = 0;
    const char *data = xrs_string_arg(args[1], &len);
    if (!data)
        return XR_CFUNC_DONE;
    return xr_file_write_once(X, XR_TO_INT(args[0]), data, len, XR_TO_INT(args[2]), result);
}

/* ========== File Operations ========== */

// remove(path) - Remove file
static XrValue io_remove(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

#ifdef XR_OS_WINDOWS
    /* A read-only attribute blocks deletion. Clearing it is part of asking the
     * host to delete rather than a choice the module makes, and the recursive
     * removal path has always done it, so both paths answer the same here. */
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
#endif
    return xr_bool(remove(path) == 0);
}

static XrValue io_rmdir(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

#ifdef XR_OS_WINDOWS
    return xr_bool(RemoveDirectoryA(path) != 0);
#else
    return xr_bool(rmdir(path) == 0);
#endif
}

// rename(old, new) - Rename file
static XrValue io_rename(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);

    const char *old_path = xrs_path_arg(args[0], NULL);
    const char *new_path = xrs_path_arg(args[1], NULL);
    if (!old_path || !new_path)
        return xr_bool(false);

    return xr_bool(rename(old_path, new_path) == 0);
}

// mkdir(path) - Create directory
static XrValue io_mkdir(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_mkdir(path, 0755) == 0);
}

// readDir(path) - Read directory contents
static XrValue io_readDir(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return xr_null();

    XrArray *arr = xr_array_new(xr_current_coro(X));
    if (!arr)
        return xr_null();

    XrFileDirEntries entries;
    if (!xr_file_dir_entries_read(path, &entries)) {
        xr_rc_release_value(xr_current_coro_heap(), xr_value_from_array(arr));
        return xr_null();
    }
    for (size_t i = 0; i < entries.count; i++)
        xr_array_push(arr, xrs_string_value_c(X, entries.names[i]));
    xr_file_dir_entries_release(&entries);

    return xr_value_from_array(arr);
}

// cwd() - Get current working directory
static XrValue io_cwd(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    char buf[XR_PATH_MAX];
    if (xr_fs_getcwd(buf, sizeof(buf)) == NULL) {
        return xr_null();
    }
    return xrs_string_value_c(X, buf);
}

/* ========== Extended Functions ========== */

// chdir(path) - Change working directory
static XrValue io_chdir(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_chdir(path) == 0);
}

// stat(path) - Get file stat info
// Uses stat() for regular info + lstat() to detect symlinks
static XrValue io_stat(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return xr_null();

    struct stat st;
    if (stat(path, &st) != 0)
        return xr_null();

#ifdef XR_OS_WINDOWS
    DWORD attrs = GetFileAttributesA(path);
    bool is_symlink = (attrs != INVALID_FILE_ATTRIBUTES) && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    // Use lstat to detect symlink (stat follows symlinks, lstat does not)
    struct stat lst;
    bool is_symlink = (lstat(path, &lst) == 0) && S_ISLNK(lst.st_mode);
#endif

#ifdef XR_OS_WINDOWS
    XrIoCoreStatFields fields = xr_io_core_stat_fields(
        (int64_t) st.st_size, (int64_t) st.st_mode, (int64_t) st.st_mtime, (int64_t) st.st_atime,
        (int64_t) st.st_ctime, 0, 0, (st.st_mode & _S_IFREG) != 0, (st.st_mode & _S_IFDIR) != 0,
        is_symlink);
#else
    XrIoCoreStatFields fields = xr_io_core_stat_fields(
        (int64_t) st.st_size, (int64_t) st.st_mode, (int64_t) st.st_mtime, (int64_t) st.st_atime,
        (int64_t) st.st_ctime, (int64_t) st.st_uid, (int64_t) st.st_gid, S_ISREG(st.st_mode),
        S_ISDIR(st.st_mode), is_symlink);
#endif

    extern XrValue xr_object_instance_value(XrObjectInstance * json);

    XrStdlibCache *cache = xr_stdlib_cache_get(X);
    XrClass *stat_cls = cache ? cache->io_stat_class : NULL;
    if (!stat_cls) {
        stat_cls = xr_stdlib_record_class_get(X, "io", "__FileStat");
        if (cache)
            cache->io_stat_class = stat_cls;
    }
    if (!stat_cls)
        return xr_null();
    XrObjectInstance *obj = xr_object_instance_new_with_class(xr_current_coro(X), stat_cls);
    if (!obj)
        return xr_null();

    xr_instance_set_dynamic_field(X, obj, XR_IO_CORE_STAT_SIZE, xr_int(fields.size));
    xr_instance_set_dynamic_field(X, obj, XR_IO_CORE_STAT_MODE, xr_int(fields.mode));
    xr_instance_set_dynamic_field(X, obj, XR_IO_CORE_STAT_MTIME, xr_int(fields.mtime));
    xr_instance_set_dynamic_field(X, obj, XR_IO_CORE_STAT_ATIME, xr_int(fields.atime));
    xr_instance_set_dynamic_field(X, obj, XR_IO_CORE_STAT_CTIME, xr_int(fields.ctime));
    xr_instance_set_dynamic_field(X, obj, XR_IO_CORE_STAT_UID, xr_int(fields.uid));
    xr_instance_set_dynamic_field(X, obj, XR_IO_CORE_STAT_GID, xr_int(fields.gid));
    xr_instance_set_dynamic_field(X, obj, XR_IO_CORE_STAT_IS_FILE, xr_bool(fields.is_file));
    xr_instance_set_dynamic_field(X, obj, XR_IO_CORE_STAT_IS_DIR, xr_bool(fields.is_dir));
    xr_instance_set_dynamic_field(X, obj, XR_IO_CORE_STAT_IS_SYMLINK, xr_bool(fields.is_symlink));

    return xr_object_instance_value(obj);
}

static XrValue io_utime_now(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

#ifdef XR_OS_WINDOWS
    return xr_bool(_utime(path, NULL) == 0);
#else
    return xr_bool(utime(path, NULL) == 0);
#endif
}

static XrValue io_chmod(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);
    if (!XR_IS_INT(args[1]))
        return xr_bool(false);

    int mode = 0;
    if (!xr_io_core_chmod_mode(XR_TO_INT(args[1]), &mode))
        return xr_bool(false);
#ifdef XR_OS_WINDOWS
    return xr_bool(_chmod(path, mode) == 0);
#else
    return xr_bool(chmod(path, mode) == 0);
#endif
}

// touch(path) - Create empty file or update timestamp
static XrValue io_symlink(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);
    const char *target = xrs_path_arg(args[0], NULL);
    const char *path = xrs_path_arg(args[1], NULL);
    if (!target || !path)
        return xr_bool(false);

#ifdef XR_OS_WINDOWS
    DWORD flags = 0;
    DWORD attrs = GetFileAttributesA(target);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    return xr_bool(CreateSymbolicLinkA(path, target, flags) != 0);
#else
    return xr_bool(symlink(target, path) == 0);
#endif
}

// readlink(path) - Read symbolic link target
static XrValue io_readlink(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return xr_null();

    char buf[XR_PATH_MAX];
#ifdef XR_OS_WINDOWS
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return xr_null();
    DWORD len = GetFinalPathNameByHandleA(h, buf, sizeof(buf), FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (len == 0 || len >= sizeof(buf))
        return xr_null();
    XrIoCorePathView view;
    if (!xr_io_core_path_result_view(buf, (size_t) len, &view))
        return xr_null();
    return xrs_string_value_n(X, view.data, view.len);
#else
    ssize_t len = readlink(path, buf, sizeof(buf) - 1);
    if (len < 0)
        return xr_null();
    XrIoCorePathView view;
    if (!xr_io_core_path_result_view(buf, (size_t) len, &view))
        return xr_null();
    return xrs_string_value_n(X, view.data, view.len);
#endif
}

// realpath(path) - Get resolved absolute path
static XrValue io_realpath(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return xr_null();

    char resolved[XR_PATH_MAX];
    if (xr_fs_realpath(path, resolved, sizeof(resolved)) == NULL)
        return xr_null();
    XrIoCorePathView view;
    if (!xr_io_core_path_result_cstr_view(resolved, &view))
        return xr_null();
    return xrs_string_value_n(X, view.data, view.len);
}

// tempFile() - Create temporary file, return path
/*
 * Create a uniquely named entry inside a caller-chosen root. Only the atomic
 * creation stays here: choosing the root and joining the name are the module's
 * own decisions and live in its Xray body, so both platforms are handed the
 * same root instead of each consulting the environment its own way.
 */
static XrValue io_make_temp_dir(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    size_t root_len = 0;
    const char *root = xrs_string_arg(args[0], &root_len);
    char tpl[XR_PATH_MAX];
    if (!xr_file_temp_template(root, tpl, sizeof(tpl)))
        return xr_null();

#ifdef XR_OS_WINDOWS
    char name[XR_PATH_MAX];
    if (GetTempFileNameA(root, "xr_", 0, name) == 0)
        return xr_null();
    DeleteFileA(name);
    if (!CreateDirectoryA(name, NULL))
        return xr_null();
    return xrs_string_value_c(X, name);
#else
    if (mkdtemp(tpl) == NULL)
        return xr_null();
    return xrs_string_value_c(X, tpl);
#endif
}

static XrValue io_make_temp_file(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    size_t root_len = 0;
    const char *root = xrs_string_arg(args[0], &root_len);
    char tpl[XR_PATH_MAX];
    if (!xr_file_temp_template(root, tpl, sizeof(tpl)))
        return xr_null();

#ifdef XR_OS_WINDOWS
    char name[XR_PATH_MAX];
    if (GetTempFileNameA(root, "xr_", 0, name) == 0)
        return xr_null();
    return xrs_string_value_c(X, name);
#else
    int fd = mkstemp(tpl);
    if (fd < 0)
        return xr_null();
    close(fd);
    return xrs_string_value_c(X, tpl);
#endif
}

/* ========== Module Loading ========== */

#define XR_STDLIB_VM_BIND_MODULE_IO 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_IO
