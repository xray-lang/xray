/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_transport_stdio.c - MCP stdio transport implementation
 */

#include "xmcp_transport_stdio.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include <errno.h>
#include <string.h>

#ifdef XR_OS_WINDOWS
#include <io.h>
#define xmcp_read_fd _read
#define xmcp_write_fd _write
#else
#include <unistd.h>
#define xmcp_read_fd read
#define xmcp_write_fd write
#endif

#define XMCP_STDIO_READ_BUF_INIT 4096u

static bool xmcp_stdio_ensure_capacity(XmcpStdioTransport *transport, size_t needed) {
    XR_DCHECK(transport != NULL, "xmcp_stdio_ensure_capacity: NULL transport");
    if (transport->read_cap >= needed)
        return true;
    if (needed > transport->max_line + 3)
        return false;

    size_t new_cap = transport->read_cap ? transport->read_cap : XMCP_STDIO_READ_BUF_INIT;
    while (new_cap < needed) {
        if (new_cap > transport->max_line / 2) {
            new_cap = transport->max_line + 3;
            break;
        }
        new_cap *= 2;
    }

    char *new_buf = xr_realloc(transport->read_buf, new_cap);
    if (!new_buf)
        return false;
    transport->read_buf = new_buf;
    transport->read_cap = new_cap;
    return true;
}

static char *xmcp_stdio_extract_line(XmcpStdioTransport *transport, size_t line_len,
                                     size_t consumed) {
    XR_DCHECK(transport != NULL, "xmcp_stdio_extract_line: NULL transport");
    char *line = xr_malloc(line_len + 1);
    if (!line)
        return NULL;
    memcpy(line, transport->read_buf, line_len);
    line[line_len] = '\0';

    size_t remaining = transport->read_len - consumed;
    if (remaining > 0)
        memmove(transport->read_buf, transport->read_buf + consumed, remaining);
    transport->read_len = remaining;
    if (transport->read_buf && transport->read_cap > transport->read_len)
        transport->read_buf[transport->read_len] = '\0';
    return line;
}

XR_FUNC bool xmcp_stdio_init(XmcpStdioTransport *transport, int read_fd, int write_fd,
                             size_t max_line) {
    XR_DCHECK(transport != NULL, "xmcp_stdio_init: NULL transport");
    memset(transport, 0, sizeof(*transport));
    transport->read_fd = read_fd;
    transport->write_fd = write_fd;
    transport->max_line = max_line ? max_line : XMCP_STDIO_MAX_LINE;
    transport->read_cap = XMCP_STDIO_READ_BUF_INIT;
    transport->read_buf = xr_malloc(transport->read_cap);
    if (!transport->read_buf)
        return false;
    transport->read_buf[0] = '\0';
    return true;
}

XR_FUNC void xmcp_stdio_destroy(XmcpStdioTransport *transport) {
    if (!transport)
        return;
    xr_free(transport->read_buf);
    transport->read_buf = NULL;
    transport->read_cap = 0;
    transport->read_len = 0;
}

XR_FUNC XmcpStdioReadStatus xmcp_stdio_read_message(XmcpStdioTransport *transport,
                                                    char **out_message, size_t *out_len) {
    XR_DCHECK(transport != NULL, "xmcp_stdio_read_message: NULL transport");
    XR_DCHECK(out_message != NULL, "xmcp_stdio_read_message: NULL out_message");
    XR_DCHECK(out_len != NULL, "xmcp_stdio_read_message: NULL out_len");
    *out_message = NULL;
    *out_len = 0;

    for (;;) {
        void *newline = memchr(transport->read_buf, '\n', transport->read_len);
        if (newline) {
            size_t pos = (size_t) ((char *) newline - transport->read_buf);
            size_t line_len = pos;
            if (line_len > 0 && transport->read_buf[line_len - 1] == '\r')
                line_len--;
            if (line_len > transport->max_line)
                return XMCP_STDIO_READ_TOO_LARGE;
            char *line = xmcp_stdio_extract_line(transport, line_len, pos + 1);
            if (!line)
                return XMCP_STDIO_READ_ERROR;
            *out_message = line;
            *out_len = line_len;
            return XMCP_STDIO_READ_OK;
        }

        if (transport->read_len > transport->max_line + 1)
            return XMCP_STDIO_READ_TOO_LARGE;

        size_t needed = transport->read_len + 1024 + 1;
        if (needed > transport->max_line + 3)
            needed = transport->max_line + 3;
        if (!xmcp_stdio_ensure_capacity(transport, needed))
            return XMCP_STDIO_READ_TOO_LARGE;

        ssize_t n = xmcp_read_fd(transport->read_fd, transport->read_buf + transport->read_len,
                                 transport->read_cap - transport->read_len - 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return XMCP_STDIO_READ_ERROR;
        }
        if (n == 0) {
            if (transport->read_len == 0)
                return XMCP_STDIO_READ_EOF;
            return XMCP_STDIO_READ_ERROR;
        }
        transport->read_len += (size_t) n;
        transport->read_buf[transport->read_len] = '\0';
    }
}

XR_FUNC bool xmcp_stdio_write_message(XmcpStdioTransport *transport, const char *json, size_t len) {
    XR_DCHECK(transport != NULL, "xmcp_stdio_write_message: NULL transport");
    XR_DCHECK(json != NULL, "xmcp_stdio_write_message: NULL json");
    if (memchr(json, '\n', len) != NULL || memchr(json, '\r', len) != NULL)
        return false;

    size_t total = 0;
    while (total < len) {
        ssize_t n = xmcp_write_fd(transport->write_fd, json + total, len - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        total += (size_t) n;
    }

    const char newline = '\n';
    total = 0;
    while (total < 1) {
        ssize_t n = xmcp_write_fd(transport->write_fd, &newline + total, 1 - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        total += (size_t) n;
    }
    return true;
}
