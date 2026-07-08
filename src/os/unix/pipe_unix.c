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

static int set_nonblocking(int fd, bool nonblocking, int *out_old_flags) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    if (out_old_flags) {
        *out_old_flags = flags;
    }
    int next = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (next == flags) {
        return 0;
    }
    return fcntl(fd, F_SETFL, next);
}

static void restore_flags(int fd, int old_flags) {
    if (old_flags >= 0) {
        (void) fcntl(fd, F_SETFL, old_flags);
    }
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

XrPipeIoStatus xr_pipe_try_read(XrPipeHandle handle, void *buf, size_t len, int64_t *out_n) {
    if (out_n) {
        *out_n = -1;
    }
    if (handle == XR_PIPE_INVALID || (!buf && len > 0) || !out_n) {
        return XR_PIPE_IO_ERROR;
    }
    int fd = (int) handle;
    int old_flags = -1;
    if (set_nonblocking(fd, true, &old_flags) != 0) {
        return XR_PIPE_IO_ERROR;
    }

    ssize_t n;
    int saved_errno = 0;
    do {
        n = read(fd, buf, len);
        saved_errno = errno;
    } while (n < 0 && saved_errno == EINTR);

    restore_flags(fd, old_flags);
    if (n >= 0) {
        *out_n = (int64_t) n;
        return XR_PIPE_IO_OK;
    }
    if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
        return XR_PIPE_IO_WOULD_BLOCK;
    }
    return XR_PIPE_IO_ERROR;
}

XrPipeIoStatus xr_pipe_try_write(XrPipeHandle handle, const void *buf, size_t len, int64_t *out_n) {
    if (out_n) {
        *out_n = -1;
    }
    if (handle == XR_PIPE_INVALID || (!buf && len > 0) || !out_n) {
        return XR_PIPE_IO_ERROR;
    }
    int fd = (int) handle;
    int old_flags = -1;
    if (set_nonblocking(fd, true, &old_flags) != 0) {
        return XR_PIPE_IO_ERROR;
    }

    ssize_t n;
    int saved_errno = 0;
    do {
        n = write(fd, buf, len);
        saved_errno = errno;
    } while (n < 0 && saved_errno == EINTR);

    restore_flags(fd, old_flags);
    if (n >= 0) {
        *out_n = (int64_t) n;
        return XR_PIPE_IO_OK;
    }
    if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
        return XR_PIPE_IO_WOULD_BLOCK;
    }
    return XR_PIPE_IO_ERROR;
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
