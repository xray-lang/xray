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
#include "../../src/os/os_dir.h"
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
static void io_release_array(XrArray *arr);

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

#ifdef XR_OS_WINDOWS
typedef int io_fd_ssize_t;
#define io_fd_open _open
#define io_fd_read _read
#define io_fd_write _write
#define io_fd_close _close
#define IO_FD_OPEN_FLAGS (_O_BINARY | _O_NOINHERIT)
#define IO_FD_CREATE_MODE (_S_IREAD | _S_IWRITE)
#else
typedef ssize_t io_fd_ssize_t;
#define io_fd_open open
#define io_fd_read read
#define io_fd_write write
#define io_fd_close close
#define IO_FD_OPEN_FLAGS O_CLOEXEC
#define IO_FD_CREATE_MODE 0644
#endif

// One open for both directions; `flags` carries the whole difference. An open
// must never hand back 0, 1 or 2, because a file landing on one of those would
// make a later write to "stdout" reach that file instead -- only reachable when
// the process starts with a standard stream already closed, which is why a
// failed lift refuses the open rather than returning the low descriptor. The
// mode argument is ignored by open(2) unless O_CREAT is among the flags.
static int XR_IO_CORE_ACQUIRE_HANDLE("xray_file_descriptor")
    io_fd_open_checked(const char *path, int flags) {
    if (!path || path[0] == '\0')
        return -1;
    int fd = io_fd_open(path, flags, IO_FD_CREATE_MODE);
    if (fd < 0 || fd > XR_IO_STD_ERR)
        return fd;
#ifdef XR_OS_WINDOWS
    int lifted = _dup(fd);
    if (lifted >= 0 && lifted <= XR_IO_STD_ERR) {
        _close(lifted);
        lifted = -1;
    }
#else
    int lifted = fcntl(fd, F_DUPFD_CLOEXEC, XR_IO_STD_ERR + 1);
#endif
    io_fd_close(fd);
    return lifted;
}

static bool io_prepare_binary_stdin(void) {
#ifdef XR_OS_WINDOWS
    return _setmode(_fileno(stdin), _O_BINARY) != -1;
#else
    return true;
#endif
}

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

static XrValue io_readStdin(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    size_t len = 0;
    char *buf = xr_io_read_stdin_all(&len);
    if (!buf)
        return xr_null();

    XrValue value = xrs_string_value_n(X, buf, len);
    xr_free(buf);
    return value;
}

static XrValue io_stream_read_bytes(XrVMRuntime *X, FILE *stream, int64_t max_bytes) {
    if (!stream || max_bytes < 0 || max_bytes > INT32_MAX)
        return xr_null();
    XrArray *arr = xr_byte_array_new(xr_current_coro(X), (int32_t) max_bytes);
    if (!arr)
        return xr_null();
    if (max_bytes == 0)
        return xr_value_from_array(arr);
    size_t count = fread(arr->data, 1, (size_t) max_bytes, stream);
    if (count == 0 && ferror(stream)) {
        io_release_array(arr);
        return xr_null();
    }
    arr->length = (int32_t) count;
    return xr_value_from_array(arr);
}

static XrValue io_readStdinBytes(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    if (!io_prepare_binary_stdin())
        return xr_null();
    size_t len = 0;
    char *buf = xr_io_read_stdin_all(&len);
    if (!buf)
        return xr_null();
    XrArray *arr = xr_byte_array_new(xr_current_coro(X), (int32_t) len);
    if (!arr) {
        xr_free(buf);
        return xr_null();
    }
    if (len > 0)
        memcpy(arr->data, buf, len);
    arr->length = (int32_t) len;
    xr_free(buf);
    return xr_value_from_array(arr);
}

static bool io_file_close_handle(int handle XR_IO_CORE_RELEASE_HANDLE("xray_file_descriptor")) {
    return io_fd_close(handle) == 0;
}

static XrValue io_fileOpen(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(-1);
    const char *path = xrs_path_arg(args[0], NULL);
    return xr_int((int64_t) io_fd_open_checked(path, O_RDONLY | IO_FD_OPEN_FLAGS));
}

static XrValue io_fileOpenWrite(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 2)
        return xr_int(-1);
    const char *path = xrs_path_arg(args[0], NULL);
    int flags = O_WRONLY | O_CREAT | IO_FD_OPEN_FLAGS | (XR_IS_TRUE(args[1]) ? O_APPEND : O_TRUNC);
    return xr_int((int64_t) io_fd_open_checked(path, flags));
}

static XrValue io_fileRead(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_null();
    int64_t handle = XR_TO_INT(args[0]);
    int64_t max_bytes = XR_TO_INT(args[1]);
    if (handle < 0 || max_bytes < 0 || max_bytes > INT32_MAX)
        return xr_null();
    if (handle == XR_IO_STD_IN) {
        if (!io_prepare_binary_stdin())
            return xr_null();
        return io_stream_read_bytes(X, stdin, max_bytes);
    }
    XrArray *arr = xr_byte_array_new(xr_current_coro(X), (int32_t) max_bytes);
    if (!arr)
        return xr_null();
    if (max_bytes == 0)
        return xr_value_from_array(arr);
    io_fd_ssize_t count;
    do {
        count = io_fd_read((int) handle, arr->data, (size_t) max_bytes);
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
        io_release_array(arr);
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
    return xr_bool(io_file_close_handle((int) handle));
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
    FILE_IO_READ_STRING
} FileIoKind;

typedef struct {
    int fd;
    XrPollDesc *pd;
    char *rbuf;  // owned raw buffer (freed on completion)
    size_t len;
    size_t off;
    FileIoKind kind;
} FileIoState;

static XrCFuncResult file_io_step(XrVMRuntime *X, FileIoState *st, XrValue *result);

static XrCFuncResult file_io_finish(XrVMRuntime *X, FileIoState *st, bool ok, XrValue *result) {
    XrValue r;
    if (!ok) {
        r = xr_null();
    } else if (st->kind == FILE_IO_READ_STRING) {
        r = xrs_string_value_n(X, st->rbuf, st->off);
    } else {  // FILE_IO_READ_BYTES
        XrArray *arr = xr_byte_array_new(xr_current_coro(X), (int32_t) st->off);
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
// finish the remaining read synchronously with pread at the offset.
static XrCFuncResult file_io_sync_rest(XrVMRuntime *X, FileIoState *st, XrValue *result) {
    while (st->off < st->len) {
        ssize_t n = pread(st->fd, st->rbuf + st->off, st->len - st->off, st->off);
        if (n > 0) {
            st->off += (size_t) n;
            continue;
        }
        if (n == 0)
            break;  // EOF
        if (errno == EINTR)
            continue;
        return file_io_finish(X, st, true, result);  // keep what was already read
    }
    return file_io_finish(X, st, true, result);
}

static XrCFuncResult file_io_complete(XrVMRuntime *X, int status, XrValue resume_value, void *ctx,
                                      XrValue *result) {
    (void) status;
    (void) resume_value;
    FileIoState *st = (FileIoState *) ctx;
    XrUringXferKind kind;
    long n = xr_netpoll_uring_xfer_result(st->pd, XR_POLL_READ, &kind);
    if (kind != XR_URING_XFER_DATA)
        return file_io_finish(X, st, st->off > 0, result);
    if (n > 0)
        st->off += (size_t) n;
    if (n > 0 && st->off < st->len)
        return file_io_step(X, st, result);  // more to transfer
    return file_io_finish(X, st, true, result);
}

static XrCFuncResult file_io_step(XrVMRuntime *X, FileIoState *st, XrValue *result) {
    XrUringReq req = {
        .kind = XR_URING_OP_FILE_READ,
        .buf = (void *) (st->rbuf + st->off),
        .len = (unsigned) (st->len - st->off),
        .offset = st->off,
    };
    XrCFuncResult cr;
    if (xr_yield_for_uring_io(X, st->pd, XR_POLL_READ, &req, file_io_complete, st, result, &cr))
        return cr;
    return file_io_sync_rest(X, st, result);  // SQ exhausted — finish synchronously
}

// Try the io_uring completion path. Returns true (and sets *out) if taken;
// false if the caller should run the synchronous fopen/fread path. `rbuf` is an
// owned buffer adopted by the state.
static bool file_io_try_uring(XrVMRuntime *X, int fd, FileIoKind kind, char *rbuf, size_t len,
                              XrValue *result, XrCFuncResult *out) {
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
    st->len = len;
    *out = (len == 0) ? file_io_finish(X, st, true, result) : file_io_step(X, st, result);
    return true;
}

/* One write submitted through io_uring. Unlike the read state machine above,
 * this owns neither the descriptor nor the buffer: __fileWrite reports what a
 * single write accepted and the retry loop that turns a short write into a
 * complete one lives in io.xr. */
typedef struct {
    XrPollDesc *pd;
} IoWriteOnceState;

static XrCFuncResult io_write_once_complete(XrVMRuntime *X, int status, XrValue resume_value,
                                            void *ctx, XrValue *result) {
    (void) status;
    (void) resume_value;
    IoWriteOnceState *st = (IoWriteOnceState *) ctx;
    XrUringXferKind kind;
    long n = xr_netpoll_uring_xfer_result(st->pd, XR_POLL_WRITE, &kind);
    bool ok = (kind == XR_URING_XFER_DATA && n >= 0);
    if (st->pd)
        xr_netpoll_close(&((XrRuntime *) X->vm.scheduler)->netpoll, st->pd);
    xr_free(st);
    *result = xr_int(ok ? (int64_t) n : -1);
    return XR_CFUNC_DONE;
}

// Returns true (and sets *out) when the submission was taken; false when the
// caller should fall back to the synchronous write(2) path.
static bool io_write_once_try_uring(XrVMRuntime *X, int fd, const char *data, size_t len,
                                    XrValue *result, XrCFuncResult *out) {
    XrRuntime *rt = (XrRuntime *) X->vm.scheduler;
    if (!rt || !xr_current_coro(X) || !xr_netpoll_uring_active(&rt->netpoll))
        return false;
    XrPollDesc *pd = xr_netpoll_open(&rt->netpoll, fd);
    if (!pd)
        return false;
    IoWriteOnceState *st = (IoWriteOnceState *) xr_calloc(1, sizeof(IoWriteOnceState));
    if (!st) {
        xr_netpoll_close(&rt->netpoll, pd);
        return false;
    }
    st->pd = pd;
    // Offset -1 means "use the descriptor's own file position". An explicit
    // offset would ignore O_APPEND and rewrite from the same place on every
    // call, which is exactly what a sequential drain must not do.
    XrUringReq req = {
        .kind = XR_URING_OP_FILE_WRITE,
        .buf = (void *) data,
        .len = (unsigned) (len > UINT_MAX ? UINT_MAX : len),
        .offset = (uint64_t) -1,
    };
    XrCFuncResult cr;
    if (xr_yield_for_uring_io(X, pd, XR_POLL_WRITE, &req, io_write_once_complete, st, result,
                              &cr)) {
        *out = cr;
        return true;
    }
    xr_netpoll_close(&rt->netpoll, pd);  // SQ exhausted — let the caller write directly
    xr_free(st);
    return false;
}
#endif  // XR_OS_LINUX && XR_HAS_IO_URING

/* One write, reported as the number of bytes the stream accepted. `offset` is
 * an index into the caller's buffer, never a file offset: the file position is
 * the descriptor's own, so an appending handle keeps appending. */
static XrCFuncResult io_write_once(XrVMRuntime *X, int64_t handle, const char *data, size_t len,
                                   int64_t offset, XrValue *result) {
    (void) X;
    *result = xr_int(-1);
    if (handle < 0 || offset < 0 || (!data && len != 0))
        return XR_CFUNC_DONE;
    if ((uint64_t) offset >= (uint64_t) len) {
        *result = xr_int(0);
        return XR_CFUNC_DONE;
    }
    const char *from = data + offset;
    size_t remaining = len - (size_t) offset;

    if (handle == XR_IO_STD_OUT || handle == XR_IO_STD_ERR) {
        FILE *stream = (handle == XR_IO_STD_OUT) ? stdout : stderr;
        size_t n = fwrite(from, 1, remaining, stream);
        *result = (n == 0 && ferror(stream)) ? xr_int(-1) : xr_int((int64_t) n);
        return XR_CFUNC_DONE;
    }
    if (handle == XR_IO_STD_IN)
        return XR_CFUNC_DONE;  // stdin is not writable

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
    do {
        XrCFuncResult out;
        if (io_write_once_try_uring(X, (int) handle, from, remaining, result, &out))
            return out;
    } while (0);
#endif

    io_fd_ssize_t n;
    do {
        n = io_fd_write((int) handle, from, remaining);
    } while (n < 0 && errno == EINTR);
    *result = xr_int(n < 0 ? -1 : (int64_t) n);
    return XR_CFUNC_DONE;
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
    return io_write_once(X, XR_TO_INT(args[0]), (const char *) arr->data, (size_t) arr->length,
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
    return io_write_once(X, XR_TO_INT(args[0]), data, len, XR_TO_INT(args[2]), result);
}

// readFile(path) - Read file content (yieldable; io_uring async when available)
static XrCFuncResult io_readFile(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    *result = xr_null();
    if (argc < 1)
        return XR_CFUNC_DONE;
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path || path[0] == '\0')
        return XR_CFUNC_DONE;

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
    do {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            struct stat sb;
            if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size >= 0 &&
                sb.st_size <= IO_MAX_READ_BYTES) {
                char *buf = (char *) xr_malloc((size_t) sb.st_size + 1);
                if (buf) {
                    XrCFuncResult out;
                    if (file_io_try_uring(X, fd, FILE_IO_READ_STRING, buf, (size_t) sb.st_size,
                                          result, &out))
                        return out;
                    xr_free(buf);  // not taken — fall through to the sync path
                }
            }
            close(fd);
        }
    } while (0);
#endif

    size_t read_size = 0;
    char *buf = io_read_file_buffer_sync(path, &read_size);
    if (!buf)
        return XR_CFUNC_DONE;
    *result = xrs_string_value_n(X, buf, read_size);
    xr_free(buf);
    return XR_CFUNC_DONE;
}

// readFileBytes(path) - Read file as byte array (yieldable; io_uring async when available)
static XrCFuncResult io_readFileBytes(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    *result = xr_null();
    if (argc < 1)
        return XR_CFUNC_DONE;
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return XR_CFUNC_DONE;

#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)
    do {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            struct stat sb;
            if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size >= 0 &&
                sb.st_size <= IO_MAX_READ_BYTES) {
                char *buf = (char *) xr_malloc((size_t) sb.st_size + 1);
                if (buf) {
                    XrCFuncResult out;
                    if (file_io_try_uring(X, fd, FILE_IO_READ_BYTES, buf, (size_t) sb.st_size,
                                          result, &out))
                        return out;
                    xr_free(buf);
                }
            }
            close(fd);
        }
    } while (0);
#endif

    size_t read_size = 0;
    char *buf = io_read_file_buffer_sync(path, &read_size);
    if (!buf)
        return XR_CFUNC_DONE;
    XrArray *arr = xr_byte_array_new(xr_current_coro(X), (int32_t) read_size);
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

/* ========== File Checks ========== */

// exists(path) - Check if path exists
static XrValue io_exists(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);

    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_exists(path));
}

// isFile(path) - Check if path is a file
static XrValue io_isFile(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_is_file(path));
}

// isDir(path) - Check if path is a directory
static XrValue io_isDir(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_path_arg(args[0], NULL);
    if (!path)
        return xr_bool(false);

    return xr_bool(xr_fs_is_dir(path));
}

// fileSize(path) - Get file size
static XrValue io_fileSize(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_int(-1);
    const char *path = xrs_path_arg(args[0], NULL);
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

static bool io_dir_for_each_entry(void *ctx, const char *path, XrIoCoreDirEntryFn visit,
                                  void *visit_ctx) {
    (void) ctx;
    if (!path || !visit)
        return false;
    XrDirIter *it = xr_dir_open(path);
    if (!it)
        return false;

    XrDirEntry e;
    bool ok = true;
    while (xr_dir_next(it, &e)) {
        if (!visit(visit_ctx, e.name)) {
            ok = false;
            break;
        }
    }

    xr_dir_close(it);
    return ok;
}

typedef struct IoReadDirEmitCtx {
    XrVMRuntime *X;
    XrArray *arr;
} IoReadDirEmitCtx;

static bool io_read_dir_emit(void *ctx, const char *path) {
    IoReadDirEmitCtx *emit = (IoReadDirEmitCtx *) ctx;
    xr_array_push(emit->arr, xrs_string_value_c(emit->X, path));
    return true;
}

static void io_release_array(XrArray *arr) {
    if (arr)
        xr_rc_release_value(xr_current_coro_heap(), xr_value_from_array(arr));
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

    IoReadDirEmitCtx emit = {.X = X, .arr = arr};
    if (!xr_io_core_read_dir(path, io_dir_for_each_entry, NULL, io_read_dir_emit, &emit)) {
        io_release_array(arr);
        return xr_null();
    }

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

// copyFile(src, dst) - Copy file
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

static XrValue io_isSymlink(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    if (argc < 1)
        return xr_bool(false);
    const char *path = xrs_path_arg(args[0], NULL);
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

    XrClass *cls = xr_stdlib_record_class_get(X, "io", "__FileStat");
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

    XrClass *stat_cls = io_get_stat_class(X);
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
static bool io_temp_template(const char *root, char *out, size_t cap) {
    if (!root || root[0] == '\0')
        return false;
    int written = snprintf(out, cap, "%s/xray_XXXXXX", root);
    return written > 0 && (size_t) written < cap;
}

static XrValue io_make_temp_dir(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    size_t root_len = 0;
    const char *root = xrs_string_arg(args[0], &root_len);
    char tpl[XR_PATH_MAX];
    if (!io_temp_template(root, tpl, sizeof(tpl)))
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
    if (!io_temp_template(root, tpl, sizeof(tpl)))
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
