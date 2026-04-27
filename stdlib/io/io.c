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
 *   Synchronous filesystem wrappers. Blocking syscalls will be routed through
 *   XrAsyncPool in a follow-up change; for now, callers on a Worker thread
 *   should expect them to stall the current coroutine.
 */

#include "io.h"
#include "../common.h"
#include "../stdlib_cache.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/object/xshape.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/base/xmalloc.h"
#include "../../src/base/xchecks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../../src/os/os_fs.h"
#include "../../src/os/os_dir.h"
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <utime.h>
#include <limits.h>
#include <ftw.h>
#ifdef XR_OS_MACOS
#include <copyfile.h>
#elif defined(XR_OS_LINUX)
#include <sys/sendfile.h>
#endif

/* ========== External Declarations ========== */

struct XrCoroutine;
extern struct XrCoroutine *xr_current_coro(XrayIsolate *X);

/* ========== File Read/Write ========== */

// Upper bound for a single in-memory read. The binding exposes the file as
// either an XrString (whose length field is uint32) or an XrArray (int32
// length field) — either way we cannot surface a buffer larger than INT32_MAX
// bytes. Callers needing >2 GiB inputs must stream the file manually.
#define IO_MAX_READ_BYTES ((long) INT32_MAX)

// readFile(path) - Read file content
static XrValue io_readFile(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_null();

    XR_DCHECK(path[0] != '\0', "io_readFile: path must be non-empty after validation");
    FILE *f = fopen(path, "rb");
    if (!f)
        return xr_null();

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0 || size > IO_MAX_READ_BYTES) {
        fclose(f);
        return xr_null();
    }
    fseek(f, 0, SEEK_SET);

    char *buf = (char *) xr_malloc((size_t) size + 1);
    if (!buf) {
        fclose(f);
        return xr_null();
    }

    size_t read_size = fread(buf, 1, (size_t) size, f);
    buf[read_size] = '\0';
    fclose(f);

    XrString *str = xr_string_intern(X, buf, read_size, 0);
    xr_free(buf);
    return xr_string_value(str);
}

// readFileBytes(path) - Read file as byte array (Array<uint8>)
static XrValue io_readFileBytes(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_null();

    FILE *f = fopen(path, "rb");
    if (!f)
        return xr_null();

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0 || size > IO_MAX_READ_BYTES) {
        fclose(f);
        return xr_null();
    }
    fseek(f, 0, SEEK_SET);

    XrArray *arr = xr_array_bytes_new(xr_current_coro(X), (int32_t) size);
    if (!arr) {
        fclose(f);
        return xr_null();
    }

    size_t read_size = fread(arr->data, 1, (size_t) size, f);
    fclose(f);
    arr->length = (int32_t) read_size;

    return xr_value_from_array(arr);
}

// writeFileBytes(path, bytes) - Write byte array to file
static XrValue io_writeFileBytes(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);

    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    // Accept Array<uint8>
    if (!xr_value_is_array(args[1]))
        return xr_bool(false);
    XrArray *arr = xr_value_to_array(args[1]);
    if (!arr || arr->elem_type != XR_ELEM_U8)
        return xr_bool(false);

    FILE *f = fopen(path, "wb");
    if (!f)
        return xr_bool(false);

    size_t written = fwrite(arr->data, 1, arr->length, f);
    fclose(f);

    return xr_bool(written == (size_t) arr->length);
}

// writeFile(path, content) - Write to file
static XrValue io_writeFile(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);

    const char *path = xrs_string_arg(args[0], NULL);
    if (!path || !XR_IS_STRING(args[1]))
        return xr_bool(false);

    XrString *str = XR_TO_STRING(args[1]);
    FILE *f = fopen(path, "wb");
    if (!f)
        return xr_bool(false);

    size_t written = fwrite(str->data, 1, str->length, f);
    fclose(f);

    return xr_bool(written == str->length);
}

// appendFile(path, content) - Append to file
static XrValue io_appendFile(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);

    const char *path = xrs_string_arg(args[0], NULL);
    if (!path || !XR_IS_STRING(args[1]))
        return xr_bool(false);

    XrString *str = XR_TO_STRING(args[1]);
    FILE *f = fopen(path, "ab");
    if (!f)
        return xr_bool(false);

    size_t written = fwrite(str->data, 1, str->length, f);
    fclose(f);

    return xr_bool(written == str->length);
}

/* ========== File Checks ========== */

// exists(path) - Check if path exists
static XrValue io_exists(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_exists(path));
}

// isFile(path) - Check if path is a file
static XrValue io_isFile(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_is_file(path));
}

// isDir(path) - Check if path is a directory
static XrValue io_isDir(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_is_dir(path));
}

// fileSize(path) - Get file size
static XrValue io_fileSize(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(-1);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_int(-1);

    XrFsStat st;
    if (xr_fs_stat(path, &st) != 0)
        return xr_int(-1);
    return xr_int((int64_t) st.size);
}

/* ========== File Operations ========== */

// remove(path) - Remove file
static XrValue io_remove(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(remove(path) == 0);
}

// rename(old, new) - Rename file
static XrValue io_rename(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);

    const char *old_path = xrs_string_arg(args[0], NULL);
    const char *new_path = xrs_string_arg(args[1], NULL);
    if (!old_path || !new_path)
        return xr_bool(false);

    return xr_bool(rename(old_path, new_path) == 0);
}

// mkdir(path) - Create directory
static XrValue io_mkdir(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(mkdir(path, 0755) == 0);
}

// readDir(path) - Read directory contents
static XrValue io_readDir(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_null();

    XrDirIter *it = xr_dir_open(path);
    if (!it)
        return xr_null();

    XrArray *arr = xr_array_new(xr_current_coro(X));
    if (!arr) {
        xr_dir_close(it);
        return xr_null();
    }

    XrDirEntry e;
    while (xr_dir_next(it, &e)) {
        XrValue name = xrs_string_value_c(X, e.name);
        xr_array_push(arr, name);
    }

    xr_dir_close(it);
    return xr_value_from_array(arr);
}

// cwd() - Get current working directory
static XrValue io_cwd(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    char buf[PATH_MAX];
    if (xr_fs_getcwd(buf, sizeof(buf)) == NULL) {
        return xr_null();
    }
    return xrs_string_value_c(X, buf);
}

/* ========== Extended Functions ========== */

// chdir(path) - Change working directory
static XrValue io_chdir(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_chdir(path) == 0);
}

// copyFile(src, dst) - Copy file
static XrValue io_copyFile(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);

    const char *src = xrs_string_arg(args[0], NULL);
    const char *dst = xrs_string_arg(args[1], NULL);
    if (!src || !dst)
        return xr_bool(false);

#ifdef XR_OS_MACOS
    // macOS: use fcopyfile for kernel-level copy (zero-copy when possible)
    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0)
        return xr_bool(false);
    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        close(src_fd);
        return xr_bool(false);
    }
    int ret = fcopyfile(src_fd, dst_fd, NULL, COPYFILE_DATA);
    close(src_fd);
    close(dst_fd);
    return xr_bool(ret == 0);
#elif defined(XR_OS_LINUX)
    // Linux: use sendfile for zero-copy
    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0)
        return xr_bool(false);
    struct stat st;
    if (fstat(src_fd, &st) < 0) {
        close(src_fd);
        return xr_bool(false);
    }
    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        close(src_fd);
        return xr_bool(false);
    }
    // sendfile(2) on Linux may transfer fewer bytes than requested, either
    // because it is interrupted by a signal or because the underlying file
    // system throttled the write. Loop until we have copied the entire
    // source length or hit an unrecoverable error.
    off_t offset = 0;
    off_t remaining = st.st_size;
    int sendfile_ok = 1;
    while (remaining > 0) {
        ssize_t sent = sendfile(dst_fd, src_fd, &offset, remaining);
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR)
                continue;
            sendfile_ok = 0;
            break;
        }
        remaining -= sent;
    }
    close(src_fd);
    close(dst_fd);
    return xr_bool(sendfile_ok && remaining == 0);
#else
    FILE *fsrc = fopen(src, "rb");
    if (!fsrc)
        return xr_bool(false);

    FILE *fdst = fopen(dst, "wb");
    if (!fdst) {
        fclose(fsrc);
        return xr_bool(false);
    }

    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
        if (fwrite(buf, 1, n, fdst) != n) {
            fclose(fsrc);
            fclose(fdst);
            return xr_bool(false);
        }
    }

    fclose(fsrc);
    fclose(fdst);
    return xr_bool(true);
#endif
}

// readLines(path) - Read file by lines
static XrValue io_readLines(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_null();

    FILE *f = fopen(path, "r");
    if (!f)
        return xr_null();

    XrArray *arr = xr_array_new(xr_current_coro(X));
    if (!arr) {
        fclose(f);
        return xr_null();
    }

    // getline() allocates via the libc allocator; we must release with free()
    // (the matching deallocator), not xr_free. This is the single documented
    // exception to the xr_malloc/xr_free symmetry rule.
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) != -1) {
        // Remove trailing newline
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            len--;
        }
        XrString *str = xr_string_intern(X, line, (size_t) len, 0);
        xr_array_push(arr, xr_string_value(str));
    }
    free(line);  // libc-owned buffer, see comment above

    fclose(f);
    return xr_value_from_array(arr);
}

// isSymlink(path) - Check if path is a symlink
static XrValue io_isSymlink(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    struct stat st;
    if (lstat(path, &st) != 0)
        return xr_bool(false);
    return xr_bool(S_ISLNK(st.st_mode));
}

// Compact Shape for stat result (Level 0: all primitives, GC skips entirely).
//
// SymbolIds are allocated per-isolate, so the shape must live on the
// isolate's stdlib cache rather than on process-global state. Building it
// lazily means the cost is paid exactly once per isolate and the shape is
// reused by every subsequent io.stat() call in that isolate.
enum {
    STAT_SIZE_IDX = 0,
    STAT_MODE_IDX,
    STAT_MTIME_IDX,
    STAT_ATIME_IDX,
    STAT_CTIME_IDX,
    STAT_UID_IDX,
    STAT_GID_IDX,
    STAT_ISFILE_IDX,
    STAT_ISDIR_IDX,
    STAT_ISSYMLINK_IDX
};

// Lazily construct the stat() result shape for the given isolate and stash
// it in the per-isolate stdlib cache. Returns NULL on shape-allocation OOM.
static XrShape *io_get_stat_shape(XrayIsolate *X) {
    XrStdlibCache *cache = xr_stdlib_cache_get(X);
    if (!cache)
        return NULL;
    if (cache->io_stat_shape)
        return cache->io_stat_shape;

    static const char *const names[] = {"size", "mode", "mtime",  "atime", "ctime",
                                        "uid",  "gid",  "isFile", "isDir", "isSymlink"};
    XrShape *shape = xr_shape_new(X, 10);
    if (!shape)
        return NULL;
    XrSymbolTable *table = (XrSymbolTable *) xr_isolate_get_symbol_table(X);
    XR_DCHECK(table != NULL, "io_get_stat_shape: symbol table must exist");
    for (int i = 0; i < 10; i++) {
        SymbolId sym = xr_symbol_register_in_table(table, names[i]);
        shape = xr_shape_transition(X, shape, sym);
        if (!shape)
            return NULL;
    }
    cache->io_stat_shape = shape;
    return shape;
}

// stat(path) - Get file stat info
// Uses stat() for regular info + lstat() to detect symlinks
static XrValue io_stat(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_null();

    struct stat st;
    if (stat(path, &st) != 0)
        return xr_null();

    // Use lstat to detect symlink (stat follows symlinks, lstat does not)
    struct stat lst;
    bool is_symlink = (lstat(path, &lst) == 0) && S_ISLNK(lst.st_mode);

    extern XrValue xr_json_value(XrJson * json);

    XrShape *shape = io_get_stat_shape(X);
    if (!shape)
        return xr_null();
    XrJson *obj = xr_json_new_with_shape(xr_current_coro(X), shape);
    if (!obj)
        return xr_null();

    obj->fields[STAT_SIZE_IDX] = xr_int((int64_t) st.st_size);
    obj->fields[STAT_MODE_IDX] = xr_int((int64_t) (st.st_mode & 0777));
    obj->fields[STAT_MTIME_IDX] = xr_int((int64_t) st.st_mtime);
    obj->fields[STAT_ATIME_IDX] = xr_int((int64_t) st.st_atime);
    obj->fields[STAT_CTIME_IDX] = xr_int((int64_t) st.st_ctime);
    obj->fields[STAT_UID_IDX] = xr_int((int64_t) st.st_uid);
    obj->fields[STAT_GID_IDX] = xr_int((int64_t) st.st_gid);
    obj->fields[STAT_ISFILE_IDX] = xr_bool(S_ISREG(st.st_mode));
    obj->fields[STAT_ISDIR_IDX] = xr_bool(S_ISDIR(st.st_mode));
    obj->fields[STAT_ISSYMLINK_IDX] = xr_bool(is_symlink);

    return xr_json_value(obj);
}

// mkdirp(path) - Recursively create directory.
// Reject empty paths up-front: the previous implementation wrote to
// tmp[-1] when handed "".
static XrValue io_mkdirp(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    // Catch truncation before we copy into a PATH_MAX buffer.
    if (path && strnlen(path, PATH_MAX) >= PATH_MAX)
        return xr_bool(false);
    if (!path || path[0] == '\0')
        return xr_bool(false);

    char tmp[PATH_MAX];
    size_t len = strnlen(path, sizeof(tmp));
    if (len == 0 || len >= sizeof(tmp))
        return xr_bool(false);
    memcpy(tmp, path, len);
    tmp[len] = '\0';
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }

    return xr_bool(mkdir(tmp, 0755) == 0 || errno == EEXIST);
}

// removeAll helper callback.
// FTW_PHYS is passed to nftw so that symlinks are *not* followed — the
// callback therefore only observes entries inside the originally supplied
// subtree, and remove() here will unlink the link itself instead of
// traversing out of tree.
static int remove_callback(const char *fpath, const struct stat *sb, int typeflag,
                           struct FTW *ftwbuf) {
    (void) sb;
    (void) typeflag;
    (void) ftwbuf;
    return remove(fpath);
}

// removeAll(path) - Recursively remove directory
static XrValue io_removeAll(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    // Use nftw to recursively traverse and remove
    int ret = nftw(path, remove_callback, 64, FTW_DEPTH | FTW_PHYS);
    return xr_bool(ret == 0);
}

// chmod(path, mode) - Change file permissions
static XrValue io_chmod(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);
    if (!XR_IS_INT(args[1]))
        return xr_bool(false);

    mode_t mode = (mode_t) XR_TO_INT(args[1]);
    return xr_bool(chmod(path, mode) == 0);
}

// touch(path) - Create empty file or update timestamp
static XrValue io_touch(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    // Try to update timestamp
    if (utime(path, NULL) == 0)
        return xr_bool(true);

    // File doesn't exist, create empty file
    FILE *f = fopen(path, "a");
    if (!f)
        return xr_bool(false);
    fclose(f);
    return xr_bool(true);
}

// symlink(target, path) - Create symbolic link
static XrValue io_symlink(XrayIsolate *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);
    const char *target = xrs_string_arg(args[0], NULL);
    const char *path = xrs_string_arg(args[1], NULL);
    if (!target || !path)
        return xr_bool(false);

    return xr_bool(symlink(target, path) == 0);
}

// readlink(path) - Read symbolic link target
static XrValue io_readlink(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_null();

    char buf[PATH_MAX];
    ssize_t len = readlink(path, buf, sizeof(buf) - 1);
    if (len < 0)
        return xr_null();
    buf[len] = '\0';
    return xrs_string_value_c(X, buf);
}

// realpath(path) - Get resolved absolute path
static XrValue io_realpath(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_null();

    char resolved[PATH_MAX];
    if (realpath(path, resolved) == NULL)
        return xr_null();
    return xrs_string_value_c(X, resolved);
}

// Determine the directory used for xray-generated temporary entries. Honours
// TMPDIR / TMP / TEMP before falling back to /tmp or the Windows temp path.
static const char *io_tempdir_root(void) {
    const char *d = getenv("TMPDIR");
    if (!d || !d[0])
        d = getenv("TMP");
    if (!d || !d[0])
        d = getenv("TEMP");
    if (!d || !d[0]) {
#ifdef XR_OS_WINDOWS
        d = "C:\\Windows\\Temp";
#else
        d = "/tmp";
#endif
    }
    return d;
}

// tempFile() - Create temporary file, return path
static XrValue io_tempFile(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    char tpl[PATH_MAX];
    int n = snprintf(tpl, sizeof(tpl), "%s/xray_XXXXXX", io_tempdir_root());
    if (n <= 0 || n >= (int) sizeof(tpl))
        return xr_null();
    int fd = mkstemp(tpl);
    if (fd < 0)
        return xr_null();
    close(fd);
    return xrs_string_value_c(X, tpl);
}

// tempDir() - Create temporary directory, return path
static XrValue io_tempDir(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    char tpl[PATH_MAX];
    int n = snprintf(tpl, sizeof(tpl), "%s/xray_XXXXXX", io_tempdir_root());
    if (n <= 0 || n >= (int) sizeof(tpl))
        return xr_null();
    if (mkdtemp(tpl) == NULL)
        return xr_null();
    return xrs_string_value_c(X, tpl);
}

// readDirRecursive helper struct
#define READ_DIR_MAX_DEPTH 64

typedef struct {
    XrayIsolate *X;
    XrArray *arr;
    const char *base;
    size_t base_len;
} ReadDirCtx;

// readDirRecursive helper function.
// Skips entries whose composed full path does not fit into PATH_MAX instead
// of silently truncating, and descends only into real directories (symlinks
// are intentionally not followed, mirroring POSIX `find -xdev` semantics).
static void read_dir_recursive_impl(ReadDirCtx *ctx, const char *path, int depth) {
    if (depth >= READ_DIR_MAX_DEPTH)
        return;
    XrDirIter *it = xr_dir_open(path);
    if (!it)
        return;

    XrDirEntry e;
    while (xr_dir_next(it, &e)) {
        char fullpath[PATH_MAX];
        int wrote = snprintf(fullpath, sizeof(fullpath), "%s/%s", path, e.name);
        if (wrote <= 0 || wrote >= (int) sizeof(fullpath)) {
            // Entry would overflow PATH_MAX; skip rather than report a
            // truncated path to the caller.
            continue;
        }

        // Add relative path
        const char *relpath = fullpath + ctx->base_len;
        if (*relpath == '/')
            relpath++;
        XrValue name = xrs_string_value_c(ctx->X, relpath);
        xr_array_push(ctx->arr, name);

        // Recursively enter real subdirectories only. xr_dir_next's
        // is_dir uses stat() as a fallback which would follow symlinks,
        // so we re-check with lstat to preserve the no-symlink-traversal
        // contract (prevents escape via bind mounts or malicious links).
        struct stat st;
        if (lstat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            read_dir_recursive_impl(ctx, fullpath, depth + 1);
        }
    }

    xr_dir_close(it);
}

// readDirRecursive(path) - Recursively read directory
static XrValue io_readDirRecursive(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_null();
    XR_DCHECK(strlen(path) < PATH_MAX, "io_readDirRecursive: path within bounds");

    XrArray *arr = xr_array_new(xr_current_coro(X));
    if (!arr)
        return xr_null();

    ReadDirCtx ctx = {.X = X, .arr = arr, .base = path, .base_len = strlen(path)};

    read_dir_recursive_impl(&ctx, path, 0);
    return xr_value_from_array(arr);
}

/* ========== Module Loading ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module io
// @handle FileStat { const size: int, const mode: int, const mtime: int, const atime: int, const
// ctime: int, const uid: int, const gid: int, const isFile: bool, const isDir: bool, const
// isSymlink: bool }

XR_DEFINE_BUILTIN(io_readFile, "readFile", "(path: string): string?", "Read entire file as string")
XR_DEFINE_BUILTIN(io_readFileBytes, "readFileBytes", "(path: string): Array<uint8>?",
                  "Read entire file as byte array")
XR_DEFINE_BUILTIN(io_writeFile, "writeFile", "(path: string, data: string): bool",
                  "Write string to file")
XR_DEFINE_BUILTIN(io_writeFileBytes, "writeFileBytes", "(path: string, data: Array<uint8>): bool",
                  "Write byte array to file")
XR_DEFINE_BUILTIN(io_appendFile, "appendFile", "(path: string, data: string): bool",
                  "Append string to file")
XR_DEFINE_BUILTIN(io_exists, "exists", "(path: string): bool", "Check if path exists")
XR_DEFINE_BUILTIN(io_isFile, "isFile", "(path: string): bool", "Check if path is a file")
XR_DEFINE_BUILTIN(io_isDir, "isDir", "(path: string): bool", "Check if path is a directory")
XR_DEFINE_BUILTIN(io_fileSize, "fileSize", "(path: string): int", "Get file size in bytes")
XR_DEFINE_BUILTIN(io_remove, "remove", "(path: string): bool", "Remove a file")
XR_DEFINE_BUILTIN(io_rename, "rename", "(old: string, new: string): bool", "Rename a file")
XR_DEFINE_BUILTIN(io_mkdir, "mkdir", "(path: string): bool", "Create directory")
XR_DEFINE_BUILTIN(io_readDir, "readDir", "(path: string): Array<string>", "List directory entries")
XR_DEFINE_BUILTIN(io_cwd, "cwd", "(): string", "Get current working directory")
XR_DEFINE_BUILTIN(io_chdir, "chdir", "(path: string): bool", "Change working directory")
XR_DEFINE_BUILTIN(io_copyFile, "copyFile", "(src: string, dst: string): bool", "Copy a file")
XR_DEFINE_BUILTIN(io_readLines, "readLines", "(path: string): Array<string>", "Read file as lines")
XR_DEFINE_BUILTIN(io_isSymlink, "isSymlink", "(path: string): bool", "Check if path is a symlink")
XR_DEFINE_BUILTIN(io_stat, "stat", "(path: string): FileStat?", "Get file stat info")
XR_DEFINE_BUILTIN(io_mkdirp, "mkdirp", "(path: string): bool", "Create directory recursively")
XR_DEFINE_BUILTIN(io_removeAll, "removeAll", "(path: string): bool", "Remove directory recursively")
XR_DEFINE_BUILTIN(io_chmod, "chmod", "(path: string, mode: int): bool", "Change file permissions")
XR_DEFINE_BUILTIN(io_touch, "touch", "(path: string): bool", "Create or update file timestamp")
XR_DEFINE_BUILTIN(io_symlink, "symlink", "(target: string, link: string): bool",
                  "Create symbolic link")
XR_DEFINE_BUILTIN(io_readlink, "readlink", "(path: string): string?", "Read symlink target")
XR_DEFINE_BUILTIN(io_realpath, "realpath", "(path: string): string?", "Resolve to absolute path")
XR_DEFINE_BUILTIN(io_tempFile, "tempFile", "(): string?", "Create temporary file")
XR_DEFINE_BUILTIN(io_tempDir, "tempDir", "(): string?", "Create temporary directory")
XR_DEFINE_BUILTIN(io_readDirRecursive, "readDirRecursive", "(path: string): Array<string>",
                  "List directory entries recursively")

XrModule *xr_load_module_io(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_io: NULL isolate");

    XrModule *mod = xr_module_create_native(isolate, "io");
    if (!mod)
        return NULL;

    // The stat() result shape is now built lazily per-isolate from
    // io_get_stat_shape() on first call, so no explicit pre-init is needed
    // at module-load time.

    // File read/write
    XRS_EXPORT(mod, isolate, "readFile", io_readFile);
    XRS_EXPORT(mod, isolate, "readFileBytes", io_readFileBytes);
    XRS_EXPORT(mod, isolate, "writeFile", io_writeFile);
    XRS_EXPORT(mod, isolate, "writeFileBytes", io_writeFileBytes);
    XRS_EXPORT(mod, isolate, "appendFile", io_appendFile);

    // File checks
    XRS_EXPORT(mod, isolate, "exists", io_exists);
    XRS_EXPORT(mod, isolate, "isFile", io_isFile);
    XRS_EXPORT(mod, isolate, "isDir", io_isDir);
    XRS_EXPORT(mod, isolate, "fileSize", io_fileSize);

    // File operations
    XRS_EXPORT(mod, isolate, "remove", io_remove);
    XRS_EXPORT(mod, isolate, "rename", io_rename);
    XRS_EXPORT(mod, isolate, "mkdir", io_mkdir);
    XRS_EXPORT(mod, isolate, "readDir", io_readDir);
    XRS_EXPORT(mod, isolate, "cwd", io_cwd);

    // Extended functions
    XRS_EXPORT(mod, isolate, "chdir", io_chdir);
    XRS_EXPORT(mod, isolate, "copyFile", io_copyFile);
    XRS_EXPORT(mod, isolate, "readLines", io_readLines);
    XRS_EXPORT(mod, isolate, "isSymlink", io_isSymlink);
    XRS_EXPORT(mod, isolate, "stat", io_stat);
    XRS_EXPORT(mod, isolate, "mkdirp", io_mkdirp);
    XRS_EXPORT(mod, isolate, "removeAll", io_removeAll);
    XRS_EXPORT(mod, isolate, "chmod", io_chmod);
    XRS_EXPORT(mod, isolate, "touch", io_touch);
    XRS_EXPORT(mod, isolate, "symlink", io_symlink);
    XRS_EXPORT(mod, isolate, "readlink", io_readlink);
    XRS_EXPORT(mod, isolate, "realpath", io_realpath);
    XRS_EXPORT(mod, isolate, "tempFile", io_tempFile);
    XRS_EXPORT(mod, isolate, "tempDir", io_tempDir);
    XRS_EXPORT(mod, isolate, "readDirRecursive", io_readDirRecursive);

    // Mark as loaded
    mod->loaded = true;
    return mod;
}
