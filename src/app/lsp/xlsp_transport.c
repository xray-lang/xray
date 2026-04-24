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
 * This matches the DAP transport layer byte-for-byte (see
 * xdap_transport.c) so that both adapters share the same correctness
 * guarantees and the same portability story.
 */

#include "xlsp_transport.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xframing.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>    // nanosleep, struct timespec

#ifdef _WIN32
#include <io.h>
#include <winsock2.h>
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
typedef int ssize_t;  // MSVC / MinGW
#else
#include <unistd.h>
#include <fcntl.h>
#endif

#define INITIAL_BUF_SIZE 4096

// ---------------------------------------------------------------------------
// Platform helpers (mirror xdap_transport.c)
// ---------------------------------------------------------------------------

static int set_nonblock(int fd) {
#ifdef _WIN32
    // Windows console stdin does not honour O_NONBLOCK via the CRT.
    // When the IDE launches us over a pipe (which VS Code does) the
    // pipe still behaves synchronously on Win32 — a proper fix needs
    // an OVERLAPPED read path via IOCP. That's out of scope for the
    // initial macOS/Linux rollout; we return success so higher layers
    // keep working, accepting occasional read() blocks on Windows until
    // the IOCP path lands alongside xpoll_iocp.
    (void)fd;
    return 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Returns: > 0 bytes read, 0 = would-block (no data yet),
//          < 0 = EOF / fatal error.
static ssize_t read_nonblock(int fd, char *buf, size_t len) {
#ifdef _WIN32
    int n = _read(fd, buf, (unsigned)len);
    if (n < 0) {
        // errno==EAGAIN on pipes set non-blocking; we also treat
        // EINTR as "try again later" so the main loop can tick.
        if (errno == EAGAIN || errno == EINTR) return 0;
        return -1;
    }
    if (n == 0) return -1;  // EOF
    return n;
#else
    ssize_t n = read(fd, buf, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR) return 0;
        return -1;
    }
    if (n == 0) return -1;  // EOF
    return n;
#endif
}

// Blocking retry loop; used only for outgoing frames. Returns total
// bytes written on success, -1 on unrecoverable error.
static ssize_t write_all(int fd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
#ifdef _WIN32
        int n = _write(fd, buf + total, (unsigned)(len - total));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return -1;
        }
#else
        ssize_t n = write(fd, buf + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Back-pressure on pipe; brief yield rather than spin.
                struct timespec ts = {0, 100000};  // 100 us
                nanosleep(&ts, NULL);
                continue;
            }
            return -1;
        }
        if (n == 0) return -1;
#endif
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static bool ensure_capacity(char **buf, size_t *cap, size_t needed) {
    if (*cap >= needed) return true;
    size_t new_cap = *cap ? *cap * 2 : INITIAL_BUF_SIZE;
    while (new_cap < needed) new_cap *= 2;
    char *tmp = *buf;
    if (!XR_REALLOC(tmp, new_cap)) return false;
    *buf = tmp;
    *cap = new_cap;
    return true;
}


// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

XrLspTransport *xlsp_transport_stdio(void) {
    XrLspTransport *t = xr_calloc(1, sizeof(XrLspTransport));
    if (!t) return NULL;

    t->read_fd  = STDIN_FILENO;
    t->write_fd = STDOUT_FILENO;
    t->pending_content_length = -1;

    t->read_buf = xr_malloc(INITIAL_BUF_SIZE);
    if (!t->read_buf) {
        xr_free(t);
        return NULL;
    }
    t->read_cap = INITIAL_BUF_SIZE;

    // stdin → non-blocking (no-op on _WIN32 for now, see set_nonblock).
    set_nonblock(t->read_fd);

    // stdout must be unbuffered so responses hit the IDE immediately.
    setvbuf(stdout, NULL, _IONBF, 0);

    t->connected = true;
    return t;
}

void xlsp_transport_free(XrLspTransport *t) {
    if (!t) return;
    // stdin/stdout intentionally not closed; they outlive the server.
    xr_free(t->read_buf);
    xr_free(t);
}

int xlsp_transport_get_fd(XrLspTransport *t) {
    return t ? t->read_fd : -1;
}

bool xlsp_transport_is_connected(XrLspTransport *t) {
    return t && t->connected;
}

char *xlsp_transport_try_read(XrLspTransport *t, size_t *out_len, bool *would_block) {
    XR_DCHECK(t != NULL, "xlsp_transport_try_read: NULL transport");
    if (!t || !t->connected) {
        if (would_block) *would_block = false;
        return NULL;
    }

    // Reserve room for one more chunk plus a terminating NUL (so
    // strstr/strcasestr stay safe on arbitrary byte input).
    if (!ensure_capacity(&t->read_buf, &t->read_cap, t->read_len + 1024 + 1)) {
        // OOM — treat as fatal. Subsequent try_read() returns EOF.
        t->connected = false;
        if (would_block) *would_block = false;
        return NULL;
    }

    ssize_t n = read_nonblock(t->read_fd,
                              t->read_buf + t->read_len,
                              t->read_cap - t->read_len - 1);
    if (n < 0) {
        // EOF or fatal error
        t->connected = false;
        if (would_block) *would_block = false;
        return NULL;
    }
    t->read_len += (size_t)n;
    t->read_buf[t->read_len] = '\0';

    // Parse header lazily via shared framing module.
    if (t->pending_content_length < 0) {
        XrFrameStatus fs = xr_frame_parse(t->read_buf, t->read_len,
                                          &t->header_end,
                                          &t->pending_content_length);
        if (fs == XR_FRAME_ERROR) {
            t->connected = false;
            if (would_block) *would_block = false;
            return NULL;
        }
    }

    if (t->pending_content_length >= 0) {
        size_t total_needed = t->header_end + (size_t)t->pending_content_length;

        if (t->read_len >= total_needed) {
            int content_len = t->pending_content_length;

            char *body = xr_malloc((size_t)content_len + 1);
            if (!body) {
                // OOM here is recoverable — we'll surface would_block
                // so the caller retries later when memory frees up.
                if (would_block) *would_block = true;
                return NULL;
            }
            memcpy(body, t->read_buf + t->header_end, (size_t)content_len);
            body[content_len] = '\0';

            // Slide the remainder of the buffer down so the next frame
            // starts at offset 0.
            size_t remaining = t->read_len - total_needed;
            if (remaining > 0) {
                memmove(t->read_buf, t->read_buf + total_needed, remaining);
            }
            t->read_len = remaining;
            t->read_buf[t->read_len] = '\0';

            // Reset parser state for the next frame.
            t->pending_content_length = -1;
            t->header_end = 0;

            if (out_len)      *out_len = (size_t)content_len;
            if (would_block)  *would_block = false;
            return body;
        }
    }

    // No complete frame yet — tell the event loop to poll again.
    if (would_block) *would_block = true;
    return NULL;
}

void xlsp_transport_write(XrLspTransport *t, const char *json, size_t len) {
    XR_DCHECK(t != NULL, "xlsp_transport_write: NULL transport");
    if (!t || !t->connected) return;

    char header[64];
    int header_len = xr_frame_write_header(header, sizeof(header), len);
    XR_DCHECK(header_len > 0, "Content-Length header overflow");

    if (write_all(t->write_fd, header, (size_t)header_len) < 0 ||
        write_all(t->write_fd, json, len) < 0) {
        // Broken pipe; mark transport dead so try_read() eventually
        // surfaces EOF to the main loop.
        t->connected = false;
    }
}
