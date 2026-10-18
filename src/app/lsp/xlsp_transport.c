/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_transport.c - LSP transport layer (stdio, non-blocking)
 *
 * Uses a fd-based, truly non-blocking incremental frame parser. stdin
 * is set O_NONBLOCK; each call to xlsp_transport_try_read() drains
 * whatever bytes the kernel has buffered, appends them to read_buf,
 * then checks whether a whole LSP frame (header + N body bytes) has
 * arrived yet. Partial frames stay in the buffer until the next call.
 *
 * Buffered frames are consumed before another read, including after pipe EOF.
 * Windows uses byte-mode stdio and the OS pipe probe to avoid blocking the
 * event thread while a connected client has no new input.
 */

#include "xlsp_transport.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "xlsp_framing.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include "../../os/os_time.h"

#include "../../os/os_fd.h"
#include "../../os/os_pipe.h"
#include <limits.h>
#include "../../os/os_net.h"
#ifdef XR_OS_WINDOWS
#if defined(_MSC_VER)
#include <corecrt_io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif
#endif

#define INITIAL_BUF_SIZE 4096
#define TRANSPORT_MAX_HEADER XLSP_FRAME_MAX_HEADER
#define TRANSPORT_MAX_BUFFER ((size_t)XLSP_FRAME_MAX_BODY + TRANSPORT_MAX_HEADER + 1)

// ---------------------------------------------------------------------------
// Byte-oriented stdio adapters
// ---------------------------------------------------------------------------

static int set_nonblock(int fd) {
#ifdef XR_OS_WINDOWS
    // Read readiness is checked by the OS pipe boundary on each read.
    (void)fd; return 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Returns: > 0 bytes read, 0 = would-block (no data yet),
//          < 0 = EOF / fatal error.
static ssize_t read_nonblock(int fd, char *buf, size_t len) {
#ifdef XR_OS_WINDOWS
    if (fd < 0) return -1;
    int64_t count = 0;
    XrPipeIoStatus status = xr_pipe_try_read((XrPipeHandle)_get_osfhandle(fd), buf, len, &count);
    if (status == XR_PIPE_IO_WOULD_BLOCK) return 0;
    if (status == XR_PIPE_IO_OK) return count ? (ssize_t)count : -1;
    // Redirected regular files have finite synchronous reads; consoles fail closed.
    struct _stat64 info;
    if (_fstat64(fd, &info) != 0 || (info.st_mode & _S_IFMT) != _S_IFREG) return -1;
    unsigned chunk = len > INT_MAX ? (unsigned)INT_MAX : (unsigned)len;
    int n = _read(fd, buf, chunk);
    return n > 0 ? n : n == 0 ? -1 : errno == EINTR ? 0 : -1;
#else
    ssize_t n = read(fd, buf, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        if (errno == EINTR)
            return 0;
        return -1;
    }
    if (n == 0)
        return -1;  // EOF
    return n;
#endif
}

// Blocking retry loop; used only for outgoing frames. Returns total
// bytes written on success, -1 on unrecoverable error.
static ssize_t write_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
#ifdef XR_OS_WINDOWS
        unsigned chunk = len - total > INT_MAX ? (unsigned)INT_MAX : (unsigned)(len - total);
        int n = _write(fd, buf + total, chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { xr_time_sleep_ns(100000ULL); continue; }
            return -1;
        }
#else
        ssize_t n = write(fd, buf + total, len - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Back-pressure on pipe; brief yield rather than spin.
                xr_time_sleep_ns(100000ULL);  // 100 us
                continue;
            }
            return -1;
        }
#endif
        if (n == 0) return -1;
        total += (size_t) n;
    }
    return (ssize_t) total;
}

static bool ensure_capacity(char **buf, size_t *cap, size_t needed) {
    if (needed > TRANSPORT_MAX_BUFFER || *cap > TRANSPORT_MAX_BUFFER) return false;
    if (*cap >= needed) return true;
    size_t next = *cap ? *cap : INITIAL_BUF_SIZE;
    while (next < needed) next = next > TRANSPORT_MAX_BUFFER / 2 ? TRANSPORT_MAX_BUFFER : next * 2;
    char *resized = xr_realloc(*buf, next); if (!resized) return false;
    *buf = resized; *cap = next; return true;
}
static bool transport_byte_mode(int read_fd, int write_fd) {
#ifdef XR_OS_WINDOWS
    int prior = _setmode(read_fd, _O_BINARY); if (prior < 0) return false;
    if (_setmode(write_fd, _O_BINARY) < 0) { (void)_setmode(read_fd, prior); return false; }
#else
    (void)read_fd; (void)write_fd;
#endif
    return true;
}
static char *transport_buffered(XrLspTransport *t, size_t *out_len) {
    if (t->pending_content_length < -1 || t->read_len > t->read_cap) { t->connected = false; return NULL; }
    if (t->pending_content_length < 0) {
        XlspFrameStatus status = xlsp_frame_read(t->read_buf, t->read_len, &t->header_end, &t->pending_content_length);
        if (status == XLSP_FRAME_ERROR || (t->pending_content_length < 0 && t->read_len >= TRANSPORT_MAX_HEADER)) {
            t->connected = false; return NULL;
        }
    }
    if (t->pending_content_length < 0) return NULL;
    if (t->header_end > TRANSPORT_MAX_HEADER || t->header_end > t->read_len ||
        t->pending_content_length > XLSP_FRAME_MAX_BODY) { t->connected = false; return NULL; }
    size_t length = (size_t)t->pending_content_length, total = t->header_end + length;
    if (t->read_len < total) return NULL;
    char *body = xr_malloc(length + 1); if (!body) { t->connected = false; return NULL; }
    memcpy(body, t->read_buf + t->header_end, length); body[length] = 0;
    size_t remaining = t->read_len - total;
    memmove(t->read_buf, t->read_buf + total, remaining); t->read_len = remaining;
    t->pending_content_length = -1; t->header_end = 0;
    if (out_len) *out_len = length;
    return body;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

XR_FUNC XrLspTransport *xlsp_transport_stdio(void) {
    XrLspTransport *t = xr_calloc(1, sizeof(XrLspTransport));
    if (!t)
        return NULL;

    t->read_fd = xr_stdin_fd();
    t->write_fd = xr_stdout_fd();
    t->pending_content_length = -1;

    t->read_buf = xr_malloc(INITIAL_BUF_SIZE);
    if (!t->read_buf) {
        xr_free(t);
        return NULL;
    }
    t->read_cap = INITIAL_BUF_SIZE;

    if (set_nonblock(t->read_fd) != 0 || !transport_byte_mode(t->read_fd, t->write_fd)) {
        xr_free(t->read_buf); xr_free(t); return NULL;
    }

    // stdout must be unbuffered so responses hit the IDE immediately.
    setvbuf(stdout, NULL, _IONBF, 0);

    t->connected = true;
    return t;
}

XR_FUNC void xlsp_transport_free(XrLspTransport *t) {
    if (!t)
        return;
    // stdin/stdout intentionally not closed; they outlive the server.
    xr_free(t->read_buf);
    xr_free(t);
}

XR_FUNC int xlsp_transport_get_fd(XrLspTransport *t) {
    return t ? t->read_fd : -1;
}

XR_FUNC bool xlsp_transport_is_connected(XrLspTransport *t) {
    return t && t->connected;
}

XR_FUNC char *xlsp_transport_try_read(XrLspTransport *t, size_t *out_len, bool *would_block) {
    if (out_len) *out_len = 0;
    if (would_block) *would_block = false;
    if (!t || !t->connected) return NULL;
    char *body = transport_buffered(t, out_len);
    if (body || !t->connected) return body;
    if (t->read_len >= TRANSPORT_MAX_BUFFER - 1) { t->connected = false; return NULL; }
    size_t needed = t->read_len + 1025;
    if (needed > TRANSPORT_MAX_BUFFER) needed = TRANSPORT_MAX_BUFFER;
    if (!ensure_capacity(&t->read_buf, &t->read_cap, needed)) { t->connected = false; return NULL; }
    size_t room = t->read_cap - t->read_len - 1;
    size_t target = t->pending_content_length < 0 ? TRANSPORT_MAX_HEADER :
        t->header_end + (size_t)t->pending_content_length;
    if (target <= t->read_len) { t->connected = false; return NULL; }
    if (room > target - t->read_len) room = target - t->read_len;
    ssize_t count = read_nonblock(t->read_fd, t->read_buf + t->read_len, room);
    if (count < 0) { t->connected = false; return NULL; }
    t->read_len += (size_t)count; t->read_buf[t->read_len] = 0;
    body = transport_buffered(t, out_len);
    if (!body && t->connected && would_block) *would_block = true;
    return body;
}

XR_FUNC void xlsp_transport_write(XrLspTransport *t, const char *json, size_t len) {
    if (!t || !t->connected) return;
    if ((!json && len) || len > XLSP_FRAME_MAX_BODY) { t->connected = false; return; }

    char header[64];
    int header_len = xlsp_frame_header(header, sizeof(header), len);
    if (header_len <= 0) { t->connected = false; return; }

    if (write_all(t->write_fd, header, (size_t) header_len) < 0 ||
        write_all(t->write_fd, json, len) < 0) {
        // Broken pipe; mark transport dead so try_read() eventually
        // surfaces EOF to the main loop.
        t->connected = false;
    }
}
