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
#include "../../src/runtime/class/xinstance.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/base/xmalloc.h"
#include "../../src/base/xchecks.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/coro/xnetpoll.h"
#include "../../src/coro/xworker.h"
#include "../../src/shared/xr_io_core.h"
#include "../../src/shared/xr_os_core.h"
#include "../../src/vm/xvm.h"  // xr_vm_yieldable_cfunction_new (XRS_EXPORT_YIELDABLE)
#include "../../src/runtime/xisolate_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/os/os_fs.h"
#include "../../src/os/os_dir.h"
#include <errno.h>
#include <limits.h>

#ifdef XR_OS_WINDOWS
#include <io.h>
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utime.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <utime.h>
#include <ftw.h>
#ifdef XR_OS_MACOS
#include <copyfile.h>
#elif defined(XR_OS_LINUX)
#include <sys/sendfile.h>
#endif
#endif

/* ========== External Declarations ========== */

struct XrCoroutine;
extern struct XrCoroutine *xr_current_coro(XrVMRuntime *X);

/* ========== File Read/Write ========== */

// Upper bound for a single in-memory read. The binding exposes the file as
// either an XrString (whose length field is uint32) or an XrArray (int32
// length field) — either way we cannot surface a buffer larger than INT32_MAX
// bytes. Callers needing >2 GiB inputs must stream the file manually.
#define IO_MAX_READ_BYTES ((long) INT32_MAX)

static char *io_read_file_buffer_sync(const char *path, size_t *out_len);

static void *io_core_alloc(void *ctx, size_t size) {
    (void) ctx;
    return xr_malloc(size);
}

static void *io_core_realloc(void *ctx, void *ptr, size_t size) {
    (void) ctx;
    return xr_realloc(ptr, size);
}

static void io_core_free(void *ctx, void *ptr) {
    (void) ctx;
    xr_free(ptr);
}

static bool io_file_seek_end(void *ctx) {
    return fseek((FILE *) ctx, 0, SEEK_END) == 0;
}

static long io_file_tell(void *ctx) {
    return ftell((FILE *) ctx);
}

static bool io_file_seek_start(void *ctx) {
    return fseek((FILE *) ctx, 0, SEEK_SET) == 0;
}

static size_t io_file_read(void *ctx, void *buf, size_t cap) {
    return fread(buf, 1, cap, (FILE *) ctx);
}

static size_t io_file_write(void *ctx, const void *buf, size_t len) {
    return fwrite(buf, 1, len, (FILE *) ctx);
}

static bool io_file_error(void *ctx) {
    return ferror((FILE *) ctx) != 0;
}

XR_FUNC char *xr_io_read_stdin_all(size_t *out_len) {
    if (out_len)
        *out_len = 0;

    clearerr(stdin);
    return xr_io_core_read_all_stream_alloc(stdin, io_file_read, io_file_error, io_core_alloc,
                                            io_core_realloc, io_core_free, NULL, 4096,
                                            IO_MAX_READ_BYTES, out_len);
}

#if !defined(XR_OS_WINDOWS) && !defined(XR_OS_MACOS) && !defined(XR_OS_LINUX)
typedef struct IoCopyFileCtx {
    FILE *src;
    FILE *dst;
} IoCopyFileCtx;

static size_t io_copy_file_read(void *ctx, void *buf, size_t cap) {
    IoCopyFileCtx *copy_ctx = (IoCopyFileCtx *) ctx;
    return fread(buf, 1, cap, copy_ctx->src);
}

static size_t io_copy_file_write(void *ctx, const void *buf, size_t len) {
    IoCopyFileCtx *copy_ctx = (IoCopyFileCtx *) ctx;
    return fwrite(buf, 1, len, copy_ctx->dst);
}

static bool io_copy_file_error(void *ctx) {
    IoCopyFileCtx *copy_ctx = (IoCopyFileCtx *) ctx;
    return ferror(copy_ctx->src) != 0;
}
#endif

static XrValue io_readStdin(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    size_t len = 0;
    char *buf = xr_io_read_stdin_all(&len);
    if (!buf)
        return xr_null();

    XrString *str = xr_string_intern(X, buf, len, 0);
    xr_free(buf);
    return xr_string_value(str);
}

/* ========== io_uring async file I/O (Linux) ==========
 *
 * Regular files are always "ready" for epoll/kqueue, so readiness pollers cannot
 * make file I/O async — the syscall blocks the worker. io_uring can: it submits
 * a file read/write SQE and parks the coroutine until the CQE. These yieldable
 * builtins use that on Linux when io_uring is active and a coroutine is running;
 * everywhere else (Windows, macOS, the epoll fallback, or non-coroutine callers)
 * they complete synchronously via the portable fopen/fread/fwrite path below.
 */
#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)

typedef enum {
    FILE_IO_READ_BYTES,
    FILE_IO_READ_STRING,
    FILE_IO_WRITE
} FileIoKind;

typedef struct {
    int fd;
    XrPollDesc *pd;
    char *rbuf;        // read: owned raw buffer (freed on completion)
    const char *wbuf;  // write: borrowed source (rooted .xr arg)
    size_t len;
    size_t off;
    FileIoKind kind;
} FileIoState;

static XrCFuncResult file_io_step(XrVMRuntime *X, FileIoState *st, XrValue *result);

static XrCFuncResult file_io_finish(XrVMRuntime *X, FileIoState *st, bool ok, XrValue *result) {
    XrValue r;
    if (!ok) {
        r = (st->kind == FILE_IO_WRITE) ? xr_bool(false) : xr_null();
    } else if (st->kind == FILE_IO_WRITE) {
        r = xr_bool(st->off == st->len);
    } else if (st->kind == FILE_IO_READ_STRING) {
        r = xr_string_value(xr_string_intern(X, st->rbuf, st->off, 0));
    } else {  // FILE_IO_READ_BYTES
        XrArray *arr = xr_array_bytes_new(xr_current_coro(X), (int32_t) st->off);
        if (arr) {
            memcpy(arr->data, st->rbuf, st->off);
            arr->length = (int32_t) st->off;
            r = xr_value_from_array(arr);
        } else {
            r = xr_null();
        }
    }
    if (st->pd)
        xr_netpoll_close(&((XrRuntime *) X->vm.scheduler)->netpoll, st->pd);
    if (st->fd >= 0)
        close(st->fd);
    if (st->rbuf)
        xr_free(st->rbuf);
    xr_free(st);
    *result = r;
    return XR_CFUNC_DONE;
}

// Fallback used when an SQE cannot be queued (submission queue momentarily full):
// finish the remaining transfer synchronously with pread/pwrite at the offset.
static XrCFuncResult file_io_sync_rest(XrVMRuntime *X, FileIoState *st, XrValue *result) {
    bool is_write = (st->kind == FILE_IO_WRITE);
    while (st->off < st->len) {
        ssize_t n = is_write ? pwrite(st->fd, st->wbuf + st->off, st->len - st->off, st->off)
                             : pread(st->fd, st->rbuf + st->off, st->len - st->off, st->off);
        if (n > 0) {
            st->off += (size_t) n;
            continue;
        }
        if (n == 0)
            break;  // read EOF
        if (errno == EINTR)
            continue;
        return file_io_finish(X, st, !is_write, result);  // read keeps partial; write fails
    }
    return file_io_finish(X, st, true, result);
}

static XrCFuncResult file_io_complete(XrVMRuntime *X, int status, XrValue resume_value, void *ctx,
                                      XrValue *result) {
    (void) status;
    (void) resume_value;
    FileIoState *st = (FileIoState *) ctx;
    bool is_write = (st->kind == FILE_IO_WRITE);
    XrUringXferKind kind;
    long n = xr_netpoll_uring_xfer_result(st->pd, is_write ? XR_POLL_WRITE : XR_POLL_READ, &kind);
    if (kind != XR_URING_XFER_DATA)
        return file_io_finish(X, st, !is_write && st->off > 0, result);
    if (n > 0)
        st->off += (size_t) n;
    if (n > 0 && st->off < st->len)
        return file_io_step(X, st, result);  // more to transfer
    return file_io_finish(X, st, true, result);
}

static XrCFuncResult file_io_step(XrVMRuntime *X, FileIoState *st, XrValue *result) {
    bool is_write = (st->kind == FILE_IO_WRITE);
    XrUringReq req = {
        .kind = is_write ? XR_URING_OP_FILE_WRITE : XR_URING_OP_FILE_READ,
        .buf = is_write ? (void *) (st->wbuf + st->off) : (void *) (st->rbuf + st->off),
        .len = (unsigned) (st->len - st->off),
        .offset = st->off,
    };
    XrCFuncResult cr;
    if (xr_yield_for_uring_io(X, st->pd, is_write ? XR_POLL_WRITE : XR_POLL_READ, &req,
                              file_io_complete, st, result, &cr))
        return cr;
    return file_io_sync_rest(X, st, result);  // SQ exhausted — finish synchronously
}

// Try the io_uring completion path. Returns true (and sets *out) if taken;
// false if the caller should run the synchronous fopen/fread path. `rbuf` is an
// owned buffer for reads (adopted by the state); `wbuf` is a borrowed source.
static bool file_io_try_uring(XrVMRuntime *X, int fd, FileIoKind kind, char *rbuf, const char *wbuf,
                              size_t len, XrValue *result, XrCFuncResult *out) {
    XrRuntime *rt = (XrRuntime *) X->vm.scheduler;
    if (!rt || !xr_current_coro(X) || !xr_netpoll_uring_active(&rt->netpoll))
        return false;
    XrPollDesc *pd = xr_netpoll_open(&rt->netpoll, fd);
    if (!pd)
        return false;
    FileIoState *st = (FileIoState *) xr_calloc(1, sizeof(FileIoState));
    if (!st) {
        xr_netpoll_close(&rt->netpoll, pd);
        return false;
    }
    st->fd = fd;
    st->pd = pd;
    st->kind = kind;
    st->rbuf = rbuf;
    st->wbuf = wbuf;
    st->len = len;
    *out = (len == 0) ? file_io_finish(X, st, true, result) : file_io_step(X, st, result);
    return true;
}
#endif  // XR_OS_LINUX && XR_HAS_IO_URING

// readFile(path) - Read file content (yieldable; io_uring async when available)
static XrCFuncResult io_readFile(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    *result = xr_null();
    if (argc < 1)
        return XR_CFUNC_DONE;
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path || path[0] == '\0')
        return XR_CFUNC_DONE;

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
    {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            struct stat sb;
            if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size >= 0 &&
                sb.st_size <= IO_MAX_READ_BYTES) {
                char *buf = (char *) xr_malloc((size_t) sb.st_size + 1);
                if (buf) {
                    XrCFuncResult out;
                    if (file_io_try_uring(X, fd, FILE_IO_READ_STRING, buf, NULL,
                                          (size_t) sb.st_size, result, &out))
                        return out;
                    xr_free(buf);  // not taken — fall through to the sync path
                }
            }
            close(fd);
        }
    }
#endif

    size_t read_size = 0;
    char *buf = io_read_file_buffer_sync(path, &read_size);
    if (!buf)
        return XR_CFUNC_DONE;
    *result = xr_string_value(xr_string_intern(X, buf, read_size, 0));
    xr_free(buf);
    return XR_CFUNC_DONE;
}

// readFileBytes(path) - Read file as byte array (yieldable; io_uring async when available)
static XrCFuncResult io_readFileBytes(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    *result = xr_null();
    if (argc < 1)
        return XR_CFUNC_DONE;
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return XR_CFUNC_DONE;

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
    {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            struct stat sb;
            if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size >= 0 &&
                sb.st_size <= IO_MAX_READ_BYTES) {
                char *buf = (char *) xr_malloc((size_t) sb.st_size + 1);
                if (buf) {
                    XrCFuncResult out;
                    if (file_io_try_uring(X, fd, FILE_IO_READ_BYTES, buf, NULL, (size_t) sb.st_size,
                                          result, &out))
                        return out;
                    xr_free(buf);
                }
            }
            close(fd);
        }
    }
#endif

    size_t read_size = 0;
    char *buf = io_read_file_buffer_sync(path, &read_size);
    if (!buf)
        return XR_CFUNC_DONE;
    XrArray *arr = xr_array_bytes_new(xr_current_coro(X), (int32_t) read_size);
    if (!arr) {
        xr_free(buf);
        return XR_CFUNC_DONE;
    }
    if (read_size > 0)
        memcpy(arr->data, buf, read_size);
    arr->length = (int32_t) read_size;
    xr_free(buf);
    *result = xr_value_from_array(arr);
    return XR_CFUNC_DONE;
}

// writeFileBytes(path, bytes) - Write byte array (yieldable; io_uring async when available)
static XrCFuncResult io_writeFileBytes(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    (void) X;
    *result = xr_bool(false);
    if (argc < 2)
        return XR_CFUNC_DONE;
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return XR_CFUNC_DONE;
    if (!xr_value_is_array(args[1]))
        return XR_CFUNC_DONE;
    XrArray *arr = xr_value_to_array(args[1]);
    if (!arr || arr->elem_type != XR_ELEM_U8)
        return XR_CFUNC_DONE;

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
    {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) {
            XrCFuncResult out;
            if (file_io_try_uring(X, fd, FILE_IO_WRITE, NULL, (const char *) arr->data,
                                  (size_t) arr->length, result, &out))
                return out;
            close(fd);
        }
    }
#endif

    FILE *f = fopen(path, "wb");
    if (!f)
        return XR_CFUNC_DONE;
    bool ok =
        xr_io_core_write_all(f, io_file_write, io_file_error, arr->data, (size_t) arr->length);
    bool close_ok = fclose(f) == 0;
    *result = xr_bool(ok && close_ok);
    return XR_CFUNC_DONE;
}

// writeFile(path, content) - Write string (yieldable; io_uring async when available)
static XrCFuncResult io_writeFile(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    (void) X;
    *result = xr_bool(false);
    if (argc < 2)
        return XR_CFUNC_DONE;
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path || !XR_IS_STRING(args[1]))
        return XR_CFUNC_DONE;
    XrString *str = XR_TO_STRING(args[1]);

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
    {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) {
            XrCFuncResult out;
            if (file_io_try_uring(X, fd, FILE_IO_WRITE, NULL, str->data, str->length, result, &out))
                return out;
            close(fd);
        }
    }
#endif

    FILE *f = fopen(path, "wb");
    if (!f)
        return XR_CFUNC_DONE;
    bool ok = xr_io_core_write_all(f, io_file_write, io_file_error, str->data, str->length);
    bool close_ok = fclose(f) == 0;
    *result = xr_bool(ok && close_ok);
    return XR_CFUNC_DONE;
}

// appendFile(path, content) - Append to file
static XrValue io_appendFile(XrVMRuntime *X, XrValue *args, int argc) {
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

    bool ok = xr_io_core_write_all(f, io_file_write, io_file_error, str->data, str->length);
    bool close_ok = fclose(f) == 0;
    return xr_bool(ok && close_ok);
}

/* ========== File Checks ========== */

// exists(path) - Check if path exists
static XrValue io_exists(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_exists(path));
}

// isFile(path) - Check if path is a file
static XrValue io_isFile(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_is_file(path));
}

// isDir(path) - Check if path is a directory
static XrValue io_isDir(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_is_dir(path));
}

// fileSize(path) - Get file size
static XrValue io_fileSize(XrVMRuntime *X, XrValue *args, int argc) {
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
static XrValue io_remove(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(remove(path) == 0);
}

// rename(old, new) - Rename file
static XrValue io_rename(XrVMRuntime *X, XrValue *args, int argc) {
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
static XrValue io_mkdir(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_mkdir(path, 0755) == 0);
}

// readDir(path) - Read directory contents
static XrValue io_readDir(XrVMRuntime *X, XrValue *args, int argc) {
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
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_chdir(path) == 0);
}

// copyFile(src, dst) - Copy file
static XrValue io_copyFile(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);

    const char *src = xrs_string_arg(args[0], NULL);
    const char *dst = xrs_string_arg(args[1], NULL);
    if (!src || !dst)
        return xr_bool(false);

#ifdef XR_OS_WINDOWS
    return xr_bool(CopyFileA(src, dst, FALSE) != 0);
#elif defined(XR_OS_MACOS)
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

    char buf[XR_IO_CORE_COPY_BUFFER_SIZE];
    IoCopyFileCtx copy_ctx = {.src = fsrc, .dst = fdst};
    bool ok = xr_io_core_copy_stream(&copy_ctx, io_copy_file_read, io_copy_file_write,
                                     io_copy_file_error, buf, sizeof(buf));
    fclose(fsrc);
    if (fclose(fdst) != 0)
        ok = false;
    return xr_bool(ok);
#endif
}

static char *io_read_file_buffer_sync(const char *path, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!path || path[0] == '\0')
        return NULL;

    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = xr_io_core_read_sized_stream_alloc(
        f, io_file_seek_end, io_file_tell, io_file_seek_start, io_file_read, io_file_error,
        io_core_alloc, io_core_free, NULL, IO_MAX_READ_BYTES, out_len);
    fclose(f);
    return buf;
}

typedef struct IoReadLinesCtx {
    XrVMRuntime *X;
    XrArray *arr;
} IoReadLinesCtx;

static bool io_read_lines_push(void *ctx, const char *data, size_t len) {
    IoReadLinesCtx *read_ctx = (IoReadLinesCtx *) ctx;
    XrString *str = xr_string_intern(read_ctx->X, data, len, 0);
    if (!str)
        return false;
    xr_array_push(read_ctx->arr, xr_string_value(str));
    return true;
}

// readLines(path) - Read file by lines
static XrValue io_readLines(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_null();

    size_t len = 0;
    char *buf = io_read_file_buffer_sync(path, &len);
    if (!buf)
        return xr_null();

    XrArray *arr = xr_array_new(xr_current_coro(X));
    if (!arr) {
        xr_free(buf);
        return xr_null();
    }

    IoReadLinesCtx read_ctx = {X, arr};
    if (!xr_io_core_read_lines_each(buf, len, io_read_lines_push, &read_ctx)) {
        xr_free(buf);
        return xr_null();
    }

    xr_free(buf);
    return xr_value_from_array(arr);
}

// isSymlink(path) - Check if path is a symlink
static XrValue io_isSymlink(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

#ifdef XR_OS_WINDOWS
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return xr_bool(false);
    return xr_bool((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
#else
    struct stat st;
    if (lstat(path, &st) != 0)
        return xr_bool(false);
    return xr_bool(S_ISLNK(st.st_mode));
#endif
}

// Lazily construct the stat() result class chain for the given isolate
// and stash it in the per-isolate stdlib cache. Returns NULL on OOM.
static XrClass *io_get_stat_class(XrVMRuntime *X) {
    XrStdlibCache *cache = xr_stdlib_cache_get(X);
    if (!cache)
        return NULL;
    if (cache->io_stat_class)
        return cache->io_stat_class;

    XrClass *cls = xr_class_build_json_chain(X, XR_IO_CORE_STAT_FIELD_NAMES,
                                             XR_IO_CORE_STAT_FIELD_COUNT, false);
    if (!cls)
        return NULL;
    cache->io_stat_class = cls;
    return cls;
}

// stat(path) - Get file stat info
// Uses stat() for regular info + lstat() to detect symlinks
static XrValue io_stat(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_string_arg(args[0], NULL);
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

    extern XrValue xr_json_value(XrJson * json);

    XrClass *stat_cls = io_get_stat_class(X);
    if (!stat_cls)
        return xr_null();
    XrJson *obj = xr_json_new_with_class(xr_current_coro(X), stat_cls);
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

    return xr_json_value(obj);
}

static int io_mkdirp_mkdir(void *ctx, const char *path) {
    (void) ctx;
    return xr_fs_mkdir(path, 0755);
}

static bool io_mkdirp_is_dir(void *ctx, const char *path) {
    (void) ctx;
    return xr_fs_is_dir(path);
}

// mkdirp(path) - Recursively create directory.
// Reject empty paths up-front: the previous implementation wrote to
// tmp[-1] when handed "".
static XrValue io_mkdirp(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    // Catch truncation before we copy into a XR_PATH_MAX buffer.
    if (path && strnlen(path, XR_PATH_MAX) >= XR_PATH_MAX)
        return xr_bool(false);
    if (!path || path[0] == '\0')
        return xr_bool(false);

    char tmp[XR_PATH_MAX];
    size_t len = strnlen(path, sizeof(tmp));
    if (len == 0 || len >= sizeof(tmp))
        return xr_bool(false);
    memcpy(tmp, path, len);
    tmp[len] = '\0';

    return xr_bool(xr_io_core_mkdirp(tmp, io_mkdirp_mkdir, io_mkdirp_is_dir, NULL));
}

static bool io_touch_update(void *ctx, const char *path) {
    (void) ctx;
#ifdef XR_OS_WINDOWS
    return _utime(path, NULL) == 0;
#else
    return utime(path, NULL) == 0;
#endif
}

static bool io_touch_create(void *ctx, const char *path) {
    (void) ctx;
    FILE *f = fopen(path, "a");
    if (!f)
        return false;
    return fclose(f) == 0;
}

#ifdef XR_OS_WINDOWS
// Windows recursive removal using FindFirstFile/FindNextFile
static bool remove_all_impl(const char *dir) {
    char pattern[XR_PATH_MAX];
    int n = snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    if (n <= 0 || n >= (int) sizeof(pattern))
        return false;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return RemoveDirectoryA(dir) || DeleteFileA(dir);

    bool ok = true;
    do {
        if (xr_io_core_is_dot_dir_entry(fd.cFileName))
            continue;
        char child[XR_PATH_MAX];
        if (!xr_io_core_join_child_path(dir, '\\', fd.cFileName, child, sizeof(child))) {
            ok = false;
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!remove_all_impl(child))
                ok = false;
        } else {
            SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileA(child))
                ok = false;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (!RemoveDirectoryA(dir))
        ok = false;
    return ok;
}
#else
// POSIX removeAll helper callback.
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
#endif

// removeAll(path) - Recursively remove directory
static XrValue io_removeAll(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

#ifdef XR_OS_WINDOWS
    return xr_bool(remove_all_impl(path));
#else
    int ret = nftw(path, remove_callback, 64, FTW_DEPTH | FTW_PHYS);
    return xr_bool(ret == 0);
#endif
}

// chmod(path, mode) - Change file permissions
static XrValue io_chmod(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
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
static XrValue io_touch(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);
    return xr_bool(xr_io_core_touch(path, io_touch_update, io_touch_create, NULL));
}

// symlink(target, path) - Create symbolic link
static XrValue io_symlink(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_bool(false);
    const char *target = xrs_string_arg(args[0], NULL);
    const char *path = xrs_string_arg(args[1], NULL);
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
    const char *path = xrs_string_arg(args[0], NULL);
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
    const char *path = xrs_string_arg(args[0], NULL);
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

// Adapter for xr_os_core_tmpdir(); fallback ordering lives in shared core.
static const char *io_core_getenv(void *ctx, const char *name) {
    (void) ctx;
    return getenv(name);
}

// tempFile() - Create temporary file, return path
static XrValue io_tempFile(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    char tpl[XR_PATH_MAX];
#ifdef XR_OS_WINDOWS
    char tmpdir[MAX_PATH];
    if (GetTempPathA(sizeof(tmpdir), tmpdir) == 0)
        return xr_null();
    char tmpfile[MAX_PATH];
    if (GetTempFileNameA(tmpdir, "xr_", 0, tmpfile) == 0)
        return xr_null();
    snprintf(tpl, sizeof(tpl), "%s", tmpfile);
#else
    const char *root = xr_os_core_tmpdir(io_core_getenv, NULL);
    if (!xr_io_core_temp_template(root, '/', "xray_XXXXXX", tpl, sizeof(tpl)))
        return xr_null();
    int fd = mkstemp(tpl);
    if (fd < 0)
        return xr_null();
    close(fd);
#endif
    return xrs_string_value_c(X, tpl);
}

// tempDir() - Create temporary directory, return path
static XrValue io_tempDir(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    char tpl[XR_PATH_MAX];
#ifdef XR_OS_WINDOWS
    char tmpdir[MAX_PATH];
    if (GetTempPathA(sizeof(tmpdir), tmpdir) == 0)
        return xr_null();
    char tmpfile[MAX_PATH];
    if (GetTempFileNameA(tmpdir, "xr_", 0, tmpfile) == 0)
        return xr_null();
    // GetTempFileName creates a file; remove it and create dir instead
    DeleteFileA(tmpfile);
    if (!CreateDirectoryA(tmpfile, NULL))
        return xr_null();
    snprintf(tpl, sizeof(tpl), "%s", tmpfile);
#else
    const char *root = xr_os_core_tmpdir(io_core_getenv, NULL);
    if (!xr_io_core_temp_template(root, '/', "xray_XXXXXX", tpl, sizeof(tpl)))
        return xr_null();
    if (mkdtemp(tpl) == NULL)
        return xr_null();
#endif
    return xrs_string_value_c(X, tpl);
}

// readDirRecursive helper struct
#define READ_DIR_MAX_DEPTH 64

typedef struct {
    XrVMRuntime *X;
    XrArray *arr;
    const char *base;
    size_t base_len;
} ReadDirCtx;

// readDirRecursive helper function.
// Skips entries whose composed full path does not fit into XR_PATH_MAX instead
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
        char fullpath[XR_PATH_MAX];
        if (!xr_io_core_join_child_path(path, '/', e.name, fullpath, sizeof(fullpath))) {
            // Entry would overflow XR_PATH_MAX; skip rather than report a
            // truncated path to the caller.
            continue;
        }

        // Add relative path
        const char *relpath = xr_io_core_relative_path_from_base(fullpath, ctx->base_len);
        XrValue name = xrs_string_value_c(ctx->X, relpath);
        xr_array_push(ctx->arr, name);

        // Recursively enter real subdirectories only, without following
        // symlinks (prevents escape via bind mounts or malicious links).
#ifdef XR_OS_WINDOWS
        DWORD attrs = GetFileAttributesA(fullpath);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
            !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            read_dir_recursive_impl(ctx, fullpath, depth + 1);
        }
#else
        struct stat st;
        if (lstat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            read_dir_recursive_impl(ctx, fullpath, depth + 1);
        }
#endif
    }

    xr_dir_close(it);
}

// readDirRecursive(path) - Recursively read directory
static XrValue io_readDirRecursive(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    const char *path = xrs_string_arg(args[0], NULL);
    if (!path)
        return xr_null();
    XR_DCHECK(strlen(path) < XR_PATH_MAX, "io_readDirRecursive: path within bounds");

    XrArray *arr = xr_array_new(xr_current_coro(X));
    if (!arr)
        return xr_null();

    ReadDirCtx ctx = {.X = X, .arr = arr, .base = path, .base_len = strlen(path)};

    read_dir_recursive_impl(&ctx, path, 0);
    return xr_value_from_array(arr);
}

/* ========== Module Loading ========== */

#define XR_STDLIB_VM_BIND_MODULE_IO 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_IO

XR_FUNC XrModule *xr_load_module_io(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_io: NULL isolate");

    XrModule *mod = xr_module_create_native(isolate, "io");
    if (!mod)
        return NULL;

    // The stat() result class is built lazily per-isolate from
    // io_get_stat_class() on first call, so no explicit pre-init is
    // needed at module-load time.

    xr_stdlib_vm_bind_io_generated(isolate, mod);

    // Mark as loaded
    mod->loaded = true;
    return mod;
}
