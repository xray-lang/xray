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
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

typedef struct XrtPipeGuard { sigset_t previous, pipe; bool pending; } XrtPipeGuard;

static XrtIoResult io_error(int code) {
    return (XrtIoResult){true, code == EPIPE,
        code ? XRT_IO_ERRNO : XRT_IO_NONE, (uint32_t)code};
}

static int pipe_begin(XrtPipeGuard *guard) {
    sigset_t pending;
    sigemptyset(&guard->pipe);
    sigaddset(&guard->pipe, SIGPIPE);
    int code = pthread_sigmask(SIG_BLOCK, &guard->pipe, &guard->previous);
    if (code) return code;
    if (sigpending(&pending)) {
        code = errno;
        (void)pthread_sigmask(SIG_SETMASK, &guard->previous, NULL);
        return code;
    }
    guard->pending = sigismember(&pending, SIGPIPE) == 1;
    return 0;
}

static XrtIoResult pipe_end(XrtPipeGuard *guard, XrtIoResult result) {
    sigset_t pending;
    if (result.broken_pipe && !guard->pending && sigpending(&pending) == 0 &&
        sigismember(&pending, SIGPIPE) == 1) {
        int signal_number;
        int code = sigwait(&guard->pipe, &signal_number);
        if (code && !result.failed) result = io_error(code);
    }
    int code = pthread_sigmask(SIG_SETMASK, &guard->previous, NULL);
    if (code && !result.failed) result = io_error(code);
    return result;
}

XR_FUNCDEF XrtIoResult xrt_io_flush_stdout(void) {
    XrtPipeGuard guard;
    int code = pipe_begin(&guard);
    if (code) return io_error(code);
    errno = 0;
    XrtIoResult result = fflush(stdout) == 0 ? (XrtIoResult){0} : io_error(errno);
    return pipe_end(&guard, result);
}

XR_FUNCDEF XrtIoResult xrt_io_write(unsigned stream, const char *bytes, size_t length) {
    XrtPipeGuard guard;
    int code = pipe_begin(&guard);
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
