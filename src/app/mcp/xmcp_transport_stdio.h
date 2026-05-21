/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_transport_stdio.h - MCP stdio transport
 *
 * KEY CONCEPT:
 *   MCP stdio uses one JSON-RPC message per line. This transport owns
 *   byte-level buffering and newline framing only; JSON-RPC validation
 *   belongs to the protocol layer.
 */

#ifndef XMCP_TRANSPORT_STDIO_H
#define XMCP_TRANSPORT_STDIO_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stddef.h>

#define XMCP_STDIO_MAX_LINE (16u * 1024u * 1024u)

typedef enum {
    XMCP_STDIO_READ_OK,
    XMCP_STDIO_READ_EOF,
    XMCP_STDIO_READ_ERROR,
    XMCP_STDIO_READ_TOO_LARGE
} XmcpStdioReadStatus;

typedef struct XmcpStdioTransport {
    int read_fd;
    int write_fd;
    char *read_buf;
    size_t read_cap;
    size_t read_len;
    size_t max_line;
} XmcpStdioTransport;

XR_FUNC bool xmcp_stdio_init(XmcpStdioTransport *transport, int read_fd, int write_fd,
                             size_t max_line);
XR_FUNC void xmcp_stdio_destroy(XmcpStdioTransport *transport);
XR_FUNC XmcpStdioReadStatus xmcp_stdio_read_message(XmcpStdioTransport *transport,
                                                    char **out_message, size_t *out_len);
XR_FUNC bool xmcp_stdio_write_message(XmcpStdioTransport *transport, const char *json, size_t len);

#endif  // XMCP_TRANSPORT_STDIO_H
