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
 *   Provides sync file operations with optional async execution via XrAsyncPool.
 *   Large file operations can be offloaded to avoid blocking Worker threads.
 */

#include "io.h"
#include "../../src/coro/xasync.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/object/xshape.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <utime.h>
#include <limits.h>
#include <ftw.h>
#ifdef __APPLE__
#include <copyfile.h>
#elif defined(__linux__)
#include <sys/sendfile.h>
#endif

/* ========== External Declarations ========== */

struct XrCoroutine;
extern struct XrCoroutine* xr_current_coro(XrayIsolate *X);

/* ========== Helper Functions ========== */

// Get string argument
static const char* get_string_arg(XrValue v) {
    if (!XR_IS_STRING(v)) return NULL;
    return XR_TO_STRING(v)->data;
}

// Create string value
static XrValue make_string(XrayIsolate *X, const char *s) {
    if (!s) return xr_null();
    XrString *str = xr_string_intern(X, s, strlen(s), 0);
    return xr_string_value(str);
}

/* ========== File Read/Write ========== */

// readFile(path) - Read file content
static XrValue io_readFile(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return xr_null();
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_null();
    
    FILE *f = fopen(path, "rb");
    if (!f) return xr_null();
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); return xr_null(); }
    fseek(f, 0, SEEK_SET);
    
    char *buf = (char*)xr_malloc(size + 1);
    if (!buf) {
        fclose(f);
        return xr_null();
    }
    
    size_t read_size = fread(buf, 1, size, f);
    buf[read_size] = '\0';
    fclose(f);
    
    XrString *str = xr_string_intern(X, buf, read_size, 0);
    xr_free(buf);
    return xr_string_value(str);
}

// readFileBytes(path) - Read file as byte array (Array<uint8>)
static XrValue io_readFileBytes(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return xr_null();
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_null();
    
    FILE *f = fopen(path, "rb");
    if (!f) return xr_null();
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); return xr_null(); }
    fseek(f, 0, SEEK_SET);
    
    XrArray *arr = xr_array_bytes_new(xr_current_coro(X), (int32_t)size);
    if (!arr) {
        fclose(f);
        return xr_null();
    }
    
    size_t read_size = fread(arr->data, 1, size, f);
    fclose(f);
    arr->length = (int32_t)read_size;
    
    return xr_value_from_array(arr);
}

// writeFileBytes(path, bytes) - Write byte array to file
static XrValue io_writeFileBytes(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 2) return xr_bool(false);
    
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_bool(false);
    
    // Accept Array<uint8>
    if (!xr_value_is_array(args[1])) return xr_bool(false);
    XrArray *arr = xr_value_to_array(args[1]);
    if (!arr || arr->elem_type != XR_ELEM_U8) return xr_bool(false);
    
    FILE *f = fopen(path, "wb");
    if (!f) return xr_bool(false);
    
    size_t written = fwrite(arr->data, 1, arr->length, f);
    fclose(f);
    
    return xr_bool(written == (size_t)arr->length);
}

// writeFile(path, content) - Write to file
static XrValue io_writeFile(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 2) return xr_bool(false);
    
    const char *path = get_string_arg(args[0]);
    if (!path || !XR_IS_STRING(args[1])) return xr_bool(false);
    
    XrString *str = XR_TO_STRING(args[1]);
    FILE *f = fopen(path, "wb");
    if (!f) return xr_bool(false);
    
    size_t written = fwrite(str->data, 1, str->length, f);
    fclose(f);
    
    return xr_bool(written == str->length);
}

// appendFile(path, content) - Append to file
static XrValue io_appendFile(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 2) return xr_bool(false);
    
    const char *path = get_string_arg(args[0]);
    if (!path || !XR_IS_STRING(args[1])) return xr_bool(false);
    
    XrString *str = XR_TO_STRING(args[1]);
    FILE *f = fopen(path, "ab");
    if (!f) return xr_bool(false);
    
    size_t written = fwrite(str->data, 1, str->length, f);
    fclose(f);
    
    return xr_bool(written == str->length);
}

/* ========== File Checks ========== */

// exists(path) - Check if path exists
static XrValue io_exists(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 1) return xr_bool(false);
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_bool(false);
    
    struct stat st;
    return xr_bool(stat(path, &st) == 0);
}

// isFile(path) - Check if path is a file
static XrValue io_isFile(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 1) return xr_bool(false);
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_bool(false);
    
    struct stat st;
    if (stat(path, &st) != 0) return xr_bool(false);
    return xr_bool(S_ISREG(st.st_mode));
}

// isDir(path) - Check if path is a directory
static XrValue io_isDir(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 1) return xr_bool(false);
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_bool(false);
    
    struct stat st;
    if (stat(path, &st) != 0) return xr_bool(false);
    return xr_bool(S_ISDIR(st.st_mode));
}

// fileSize(path) - Get file size
static XrValue io_fileSize(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 1) return xr_int(-1);
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_int(-1);
    
    struct stat st;
    if (stat(path, &st) != 0) return xr_int(-1);
    return xr_int(st.st_size);
}

/* ========== File Operations ========== */

// remove(path) - Remove file
static XrValue io_remove(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 1) return xr_bool(false);
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_bool(false);
    
    return xr_bool(remove(path) == 0);
}

// rename(old, new) - Rename file
static XrValue io_rename(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 2) return xr_bool(false);
    
    const char *old_path = get_string_arg(args[0]);
    const char *new_path = get_string_arg(args[1]);
    if (!old_path || !new_path) return xr_bool(false);
    
    return xr_bool(rename(old_path, new_path) == 0);
}

// mkdir(path) - Create directory
static XrValue io_mkdir(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 1) return xr_bool(false);
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_bool(false);
    
    return xr_bool(mkdir(path, 0755) == 0);
}

// readDir(path) - Read directory contents
static XrValue io_readDir(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return xr_null();
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_null();
    
    DIR *dir = opendir(path);
    if (!dir) return xr_null();
    
    XrArray *arr = xr_array_new(xr_current_coro(X));
    if (!arr) {
        closedir(dir);
        return xr_null();
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        XrValue name = make_string(X, entry->d_name);
        xr_array_push(arr, name);
    }
    
    closedir(dir);
    return xr_value_from_array(arr);
}

// cwd() - Get current working directory
static XrValue io_cwd(XrayIsolate *X, XrValue *args, int argc) {
    (void)args; (void)argc;
    
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf)) == NULL) {
        return xr_null();
    }
    return make_string(X, buf);
}

/* ========== Extended Functions ========== */

// chdir(path) - Change working directory
static XrValue io_chdir(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 1) return xr_bool(false);
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_bool(false);
    
    return xr_bool(chdir(path) == 0);
}

// copyFile(src, dst) - Copy file
static XrValue io_copyFile(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 2) return xr_bool(false);
    
    const char *src = get_string_arg(args[0]);
    const char *dst = get_string_arg(args[1]);
    if (!src || !dst) return xr_bool(false);
    
#ifdef __APPLE__
    // macOS: use fcopyfile for kernel-level copy (zero-copy when possible)
    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) return xr_bool(false);
    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) { close(src_fd); return xr_bool(false); }
    int ret = fcopyfile(src_fd, dst_fd, NULL, COPYFILE_DATA);
    close(src_fd);
    close(dst_fd);
    return xr_bool(ret == 0);
#elif defined(__linux__)
    // Linux: use sendfile for zero-copy
    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) return xr_bool(false);
    struct stat st;
    if (fstat(src_fd, &st) < 0) { close(src_fd); return xr_bool(false); }
    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) { close(src_fd); return xr_bool(false); }
    off_t offset = 0;
    ssize_t sent = sendfile(dst_fd, src_fd, &offset, st.st_size);
    close(src_fd);
    close(dst_fd);
    return xr_bool(sent == st.st_size);
#else
    FILE *fsrc = fopen(src, "rb");
    if (!fsrc) return xr_bool(false);
    
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
    if (argc < 1) return xr_null();
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_null();
    
    FILE *f = fopen(path, "r");
    if (!f) return xr_null();
    
    XrArray *arr = xr_array_new(xr_current_coro(X));
    if (!arr) {
        fclose(f);
        return xr_null();
    }
    
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) != -1) {
        // Remove trailing newline
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            len--;
        }
        XrString *str = xr_string_intern(X, line, (size_t)len, 0);
        xr_array_push(arr, xr_string_value(str));
    }
    free(line);
    
    fclose(f);
    return xr_value_from_array(arr);
}

// isSymlink(path) - Check if path is a symlink
static XrValue io_isSymlink(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 1) return xr_bool(false);
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_bool(false);
    
    struct stat st;
    if (lstat(path, &st) != 0) return xr_bool(false);
    return xr_bool(S_ISLNK(st.st_mode));
}

// Compact Shape for stat result (Level 0: all primitives, GC skips entirely)
static XrShape *shape_stat_result = NULL;

enum {
    STAT_SIZE_IDX = 0, STAT_MODE_IDX, STAT_MTIME_IDX, STAT_ATIME_IDX,
    STAT_CTIME_IDX, STAT_UID_IDX, STAT_GID_IDX,
    STAT_ISFILE_IDX, STAT_ISDIR_IDX, STAT_ISSYMLINK_IDX
};

// Called once per isolate during module load.
// Must rebuild every time because SymbolIds are per-isolate.
static void io_ensure_stat_shape(XrayIsolate *X) {
    const char *names[] = {
        "size", "mode", "mtime", "atime", "ctime",
        "uid", "gid", "isFile", "isDir", "isSymlink"
    };
    XrShape *shape = xr_shape_new(X, 10);
    if (!shape) return;
    XrSymbolTable *table = (XrSymbolTable*)xr_isolate_get_symbol_table(X);
    for (int i = 0; i < 10; i++) {
        SymbolId sym = xr_symbol_register_in_table(table, names[i]);
        shape = xr_shape_transition(X, shape, sym);
        if (!shape) return;
    }
    shape_stat_result = shape;
}

// stat(path) - Get file stat info
// Uses stat() for regular info + lstat() to detect symlinks
static XrValue io_stat(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return xr_null();
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_null();
    
    struct stat st;
    if (stat(path, &st) != 0) return xr_null();
    
    // Use lstat to detect symlink (stat follows symlinks, lstat does not)
    struct stat lst;
    bool is_symlink = (lstat(path, &lst) == 0) && S_ISLNK(lst.st_mode);
    
    extern XrValue xr_json_value(XrJson *json);
    
    XrJson *obj = xr_json_new_with_shape(xr_current_coro(X), shape_stat_result);
    if (!obj) return xr_null();
    
    obj->fields[STAT_SIZE_IDX]      = xr_int((int64_t)st.st_size);
    obj->fields[STAT_MODE_IDX]      = xr_int((int64_t)(st.st_mode & 0777));
    obj->fields[STAT_MTIME_IDX]     = xr_int((int64_t)st.st_mtime);
    obj->fields[STAT_ATIME_IDX]     = xr_int((int64_t)st.st_atime);
    obj->fields[STAT_CTIME_IDX]     = xr_int((int64_t)st.st_ctime);
    obj->fields[STAT_UID_IDX]       = xr_int((int64_t)st.st_uid);
    obj->fields[STAT_GID_IDX]       = xr_int((int64_t)st.st_gid);
    obj->fields[STAT_ISFILE_IDX]    = xr_bool(S_ISREG(st.st_mode));
    obj->fields[STAT_ISDIR_IDX]     = xr_bool(S_ISDIR(st.st_mode));
    obj->fields[STAT_ISSYMLINK_IDX] = xr_bool(is_symlink);
    
    return xr_json_value(obj);
}

// mkdirp(path) - Recursively create directory
static XrValue io_mkdirp(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 1) return xr_bool(false);
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_bool(false);
    
    char tmp[PATH_MAX];
    char *p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    
    return xr_bool(mkdir(tmp, 0755) == 0 || errno == EEXIST);
}

// removeAll helper callback
static int remove_callback(const char *fpath, const struct stat *sb,
                          int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(fpath);
}

// removeAll(path) - Recursively remove directory
static XrValue io_removeAll(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 1) return xr_bool(false);
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_bool(false);
    
    // Use nftw to recursively traverse and remove
    int ret = nftw(path, remove_callback, 64, FTW_DEPTH | FTW_PHYS);
    return xr_bool(ret == 0);
}

// chmod(path, mode) - Change file permissions
static XrValue io_chmod(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 2) return xr_bool(false);
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_bool(false);
    if (!XR_IS_INT(args[1])) return xr_bool(false);
    
    mode_t mode = (mode_t)XR_TO_INT(args[1]);
    return xr_bool(chmod(path, mode) == 0);
}

// touch(path) - Create empty file or update timestamp
static XrValue io_touch(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 1) return xr_bool(false);
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_bool(false);
    
    // Try to update timestamp
    if (utime(path, NULL) == 0) return xr_bool(true);
    
    // File doesn't exist, create empty file
    FILE *f = fopen(path, "a");
    if (!f) return xr_bool(false);
    fclose(f);
    return xr_bool(true);
}

// symlink(target, path) - Create symbolic link
static XrValue io_symlink(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 2) return xr_bool(false);
    const char *target = get_string_arg(args[0]);
    const char *path = get_string_arg(args[1]);
    if (!target || !path) return xr_bool(false);
    
    return xr_bool(symlink(target, path) == 0);
}

// readlink(path) - Read symbolic link target
static XrValue io_readlink(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return xr_null();
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_null();
    
    char buf[PATH_MAX];
    ssize_t len = readlink(path, buf, sizeof(buf) - 1);
    if (len < 0) return xr_null();
    buf[len] = '\0';
    return make_string(X, buf);
}

// realpath(path) - Get resolved absolute path
static XrValue io_realpath(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return xr_null();
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_null();
    
    char resolved[PATH_MAX];
    if (realpath(path, resolved) == NULL) return xr_null();
    return make_string(X, resolved);
}

// tempFile() - Create temporary file, return path
static XrValue io_tempFile(XrayIsolate *X, XrValue *args, int argc) {
    (void)args; (void)argc;
    
    char template[] = "/tmp/xray_XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) return xr_null();
    close(fd);
    return make_string(X, template);
}

// tempDir() - Create temporary directory, return path
static XrValue io_tempDir(XrayIsolate *X, XrValue *args, int argc) {
    (void)args; (void)argc;
    
    char template[] = "/tmp/xray_XXXXXX";
    if (mkdtemp(template) == NULL) return xr_null();
    return make_string(X, template);
}

// readDirRecursive helper struct
#define READ_DIR_MAX_DEPTH 64

typedef struct {
    XrayIsolate *X;
    XrArray *arr;
    const char *base;
    size_t base_len;
} ReadDirCtx;

// readDirRecursive helper function
static void read_dir_recursive_impl(ReadDirCtx *ctx, const char *path, int depth) {
    if (depth >= READ_DIR_MAX_DEPTH) return;
    DIR *dir = opendir(path);
    if (!dir) return;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char fullpath[PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
        
        // Add relative path
        const char *relpath = fullpath + ctx->base_len;
        if (*relpath == '/') relpath++;
        XrValue name = make_string(ctx->X, relpath);
        xr_array_push(ctx->arr, name);
        
        // Recursively enter subdirectory
        struct stat st;
        if (lstat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            read_dir_recursive_impl(ctx, fullpath, depth + 1);
        }
    }
    
    closedir(dir);
}

// readDirRecursive(path) - Recursively read directory
static XrValue io_readDirRecursive(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return xr_null();
    const char *path = get_string_arg(args[0]);
    if (!path) return xr_null();
    
    XrArray *arr = xr_array_new(xr_current_coro(X));
    if (!arr) return xr_null();
    
    ReadDirCtx ctx = {
        .X = X,
        .arr = arr,
        .base = path,
        .base_len = strlen(path)
    };
    
    read_dir_recursive_impl(&ctx, path, 0);
    return xr_value_from_array(arr);
}

/* ========== XrAsyncPool Integration ========== */

// Async read file data
typedef struct {
    char path[PATH_MAX];
    char *content;
    size_t size;
    bool success;
} XrIoReadData;

// Async write file data
typedef struct {
    char path[PATH_MAX];
    char *content;
    size_t size;
    bool append;
    bool success;
} XrIoWriteData;

// Async read executor (runs in async thread)
static void io_async_read_invoke(void *data) {
    XrIoReadData *d = (XrIoReadData *)data;
    d->success = false;
    d->content = NULL;
    d->size = 0;
    
    FILE *f = fopen(d->path, "rb");
    if (!f) return;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    
    size_t read_size = fread(buf, 1, size, f);
    buf[read_size] = '\0';
    fclose(f);
    
    d->content = buf;
    d->size = read_size;
    d->success = true;
}

// Async write executor (runs in async thread)
static void io_async_write_invoke(void *data) {
    XrIoWriteData *d = (XrIoWriteData *)data;
    d->success = false;
    
    FILE *f = fopen(d->path, d->append ? "ab" : "wb");
    if (!f) return;
    
    size_t written = fwrite(d->content, 1, d->size, f);
    fclose(f);
    
    d->success = (written == d->size);
}

// xr_io_read_on_async - Read file via XrAsyncPool
// Falls back to sync read if no async pool
bool xr_io_read_on_async(XrAsyncPool *pool, struct XrCoroutine *coro,
                          int worker_id, const char *path,
                          char **content, size_t *size) {
    if (!path || !content || !size) return false;
    
    // No async pool, sync read
    if (!pool) {
        FILE *f = fopen(path, "rb");
        if (!f) return false;
        
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        char *buf = (char *)malloc(file_size + 1);
        if (!buf) {
            fclose(f);
            return false;
        }
        
        *size = fread(buf, 1, file_size, f);
        buf[*size] = '\0';
        fclose(f);
        
        *content = buf;
        return true;
    }
    
    // Create async task data
    XrIoReadData *data = (XrIoReadData *)malloc(sizeof(XrIoReadData));
    if (!data) return false;
    
    strncpy(data->path, path, sizeof(data->path) - 1);
    data->path[sizeof(data->path) - 1] = '\0';
    data->content = NULL;
    data->size = 0;
    data->success = false;
    
    // Create async job
    XrAsyncJob *job = xr_async_job_create(coro, worker_id, io_async_read_invoke, data);
    if (!job) {
        free(data);
        return false;
    }
    
    // Submit job
    xr_async_submit(pool, job);
    return true;
}

// xr_io_write_on_async - Write file via XrAsyncPool
bool xr_io_write_on_async(XrAsyncPool *pool, struct XrCoroutine *coro,
                           int worker_id, const char *path,
                           const char *content, size_t size, bool append) {
    if (!path || !content) return false;
    
    // No async pool, sync write
    if (!pool) {
        FILE *f = fopen(path, append ? "ab" : "wb");
        if (!f) return false;
        
        size_t written = fwrite(content, 1, size, f);
        fclose(f);
        return written == size;
    }
    
    // Create async task data
    XrIoWriteData *data = (XrIoWriteData *)malloc(sizeof(XrIoWriteData));
    if (!data) return false;
    
    strncpy(data->path, path, sizeof(data->path) - 1);
    data->path[sizeof(data->path) - 1] = '\0';
    
    // Copy content (async thread needs independent copy)
    data->content = (char *)malloc(size);
    if (!data->content) {
        free(data);
        return false;
    }
    memcpy(data->content, content, size);
    data->size = size;
    data->append = append;
    data->success = false;
    
    // Create async job
    XrAsyncJob *job = xr_async_job_create(coro, worker_id, io_async_write_invoke, data);
    if (!job) {
        free(data->content);
        free(data);
        return false;
    }
    
    // Submit job
    xr_async_submit(pool, job);
    return true;
}

/* ========== Module Loading ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module io
// @handle FileStat { const size: int, const mode: int, const mtime: int, const atime: int, const ctime: int, const uid: int, const gid: int, const isFile: bool, const isDir: bool, const isSymlink: bool }

XR_DEFINE_BUILTIN(io_readFile, "readFile", "(path: string): string?", "Read entire file as string")
XR_DEFINE_BUILTIN(io_readFileBytes, "readFileBytes", "(path: string): Array<uint8>?", "Read entire file as byte array")
XR_DEFINE_BUILTIN(io_writeFile, "writeFile", "(path: string, data: string): bool", "Write string to file")
XR_DEFINE_BUILTIN(io_writeFileBytes, "writeFileBytes", "(path: string, data: Array<uint8>): bool", "Write byte array to file")
XR_DEFINE_BUILTIN(io_appendFile, "appendFile", "(path: string, data: string): bool", "Append string to file")
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
XR_DEFINE_BUILTIN(io_symlink, "symlink", "(target: string, link: string): bool", "Create symbolic link")
XR_DEFINE_BUILTIN(io_readlink, "readlink", "(path: string): string?", "Read symlink target")
XR_DEFINE_BUILTIN(io_realpath, "realpath", "(path: string): string?", "Resolve to absolute path")
XR_DEFINE_BUILTIN(io_tempFile, "tempFile", "(): string?", "Create temporary file")
XR_DEFINE_BUILTIN(io_tempDir, "tempDir", "(): string?", "Create temporary directory")
XR_DEFINE_BUILTIN(io_readDirRecursive, "readDirRecursive", "(path: string): Array<string>", "List directory entries recursively")

XrModule* xr_load_module_io(XrayIsolate *isolate) {
    // Create native module
    XrModule *mod = xr_module_create_native(isolate, "io");
    if (!mod) return NULL;
    
    // Initialize stat shape once at module load (thread-safe)
    io_ensure_stat_shape(isolate);
    
    // Add exported functions
    extern XrCFunction* xr_vm_cfunction_new(XrayIsolate *isolate, XrCFunctionPtr func, const char *name);
    extern XrValue xr_value_from_cfunction(XrCFunction *cfunc);
    
    #define EXPORT_CFUNC(name_str, func_ptr) \
        do { \
            XrCFunction *cfunc = xr_vm_cfunction_new(isolate, func_ptr, name_str); \
            XrValue fn_val = xr_value_from_cfunction(cfunc); \
            xr_module_add_export(isolate, mod, name_str, fn_val); \
        } while(0)
    
    // File read/write
    EXPORT_CFUNC("readFile", io_readFile);
    EXPORT_CFUNC("readFileBytes", io_readFileBytes);
    EXPORT_CFUNC("writeFile", io_writeFile);
    EXPORT_CFUNC("writeFileBytes", io_writeFileBytes);
    EXPORT_CFUNC("appendFile", io_appendFile);
    
    // File checks
    EXPORT_CFUNC("exists", io_exists);
    EXPORT_CFUNC("isFile", io_isFile);
    EXPORT_CFUNC("isDir", io_isDir);
    EXPORT_CFUNC("fileSize", io_fileSize);
    
    // File operations
    EXPORT_CFUNC("remove", io_remove);
    EXPORT_CFUNC("rename", io_rename);
    EXPORT_CFUNC("mkdir", io_mkdir);
    EXPORT_CFUNC("readDir", io_readDir);
    EXPORT_CFUNC("cwd", io_cwd);
    
    // Extended functions
    EXPORT_CFUNC("chdir", io_chdir);
    EXPORT_CFUNC("copyFile", io_copyFile);
    EXPORT_CFUNC("readLines", io_readLines);
    EXPORT_CFUNC("isSymlink", io_isSymlink);
    EXPORT_CFUNC("stat", io_stat);
    EXPORT_CFUNC("mkdirp", io_mkdirp);
    EXPORT_CFUNC("removeAll", io_removeAll);
    EXPORT_CFUNC("chmod", io_chmod);
    EXPORT_CFUNC("touch", io_touch);
    EXPORT_CFUNC("symlink", io_symlink);
    EXPORT_CFUNC("readlink", io_readlink);
    EXPORT_CFUNC("realpath", io_realpath);
    EXPORT_CFUNC("tempFile", io_tempFile);
    EXPORT_CFUNC("tempDir", io_tempDir);
    EXPORT_CFUNC("readDirRecursive", io_readDirRecursive);
    
    #undef EXPORT_CFUNC
    
    // Mark as loaded
    mod->loaded = true;
    return mod;
}
