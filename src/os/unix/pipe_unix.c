/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * pipe_unix.c - POSIX implementation of os_pipe.h.
 */

#include "../os_pipe.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static int set_inheritable(int fd, bool inheritable) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return -1;
    }
    if (inheritable) {
        flags &= ~FD_CLOEXEC;
    } else {
        flags |= FD_CLOEXEC;
    }
    return fcntl(fd, F_SETFD, flags);
}

int xr_pipe_create(XrPipe *out, const XrPipeOptions *options) {
    if (!out) {
        return -1;
    }
    out->read = XR_PIPE_INVALID;
    out->write = XR_PIPE_INVALID;

    int fds[2];
    if (pipe(fds) != 0) {
        return -1;
    }

    bool read_inheritable = options && options->read_inheritable;
    bool write_inheritable = options && options->write_inheritable;
    if (set_inheritable(fds[0], read_inheritable) != 0 ||
        set_inheritable(fds[1], write_inheritable) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    out->read = (XrPipeHandle) fds[0];
    out->write = (XrPipeHandle) fds[1];
    return 0;
}

int xr_pipe_close(XrPipeHandle handle) {
    if (handle == XR_PIPE_INVALID) {
        return 0;
    }
    return close((int) handle) == 0 ? 0 : -1;
}

int64_t xr_pipe_read(XrPipeHandle handle, void *buf, size_t len) {
    if (handle == XR_PIPE_INVALID || (!buf && len > 0)) {
        return -1;
    }
    ssize_t n;
    do {
        n = read((int) handle, buf, len);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? -1 : (int64_t) n;
}

int64_t xr_pipe_write(XrPipeHandle handle, const void *buf, size_t len) {
    if (handle == XR_PIPE_INVALID || (!buf && len > 0)) {
        return -1;
    }
    ssize_t n;
    do {
        n = write((int) handle, buf, len);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? -1 : (int64_t) n;
}
