/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_output_unix.c - POSIX output with local SIGPIPE containment
 *
 * KEY CONCEPT:
 *   Block SIGPIPE only on the calling thread and consume only a newly raised
 *   pending pipe signal before restoring the original mask.
 */
#include "os/os_native_output.h"
#include "os/unix/xpipe_guard.inc.h"
#include <stdio.h>
#include <unistd.h>

static XrtIoResult io_error(int code) {
    return (XrtIoResult){true, code == EPIPE,
        code ? XRT_IO_ERRNO : XRT_IO_NONE, (uint32_t)code};
}

static XrtIoResult pipe_end(XrOsPipeGuard *guard, XrtIoResult result) {
    int code = xr_os_pipe_guard_end(guard, result.broken_pipe);
    return code && !result.failed ? io_error(code) : result;
}

XR_FUNCDEF XrtIoResult xrt_io_flush_stdout(void) {
    XrOsPipeGuard guard;
    int code = xr_os_pipe_guard_begin(&guard);
    if (code) return io_error(code);
    errno = 0;
    XrtIoResult result = fflush(stdout) == 0 ? (XrtIoResult){0} : io_error(errno);
    return pipe_end(&guard, result);
}

XR_FUNCDEF XrtIoResult xrt_io_write(unsigned stream, const char *bytes, size_t length) {
    XrOsPipeGuard guard;
    int code = xr_os_pipe_guard_begin(&guard);
    if (code) return io_error(code);
    XrtIoResult result = {0};
    while (length) {
        ssize_t written = write(stream == 1 ? STDOUT_FILENO : STDERR_FILENO, bytes, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            result = io_error(errno);
            break;
        }
        if (!written) { result = io_error(0); break; }
        bytes += (size_t)written;
        length -= (size_t)written;
    }
    return pipe_end(&guard, result);
}
