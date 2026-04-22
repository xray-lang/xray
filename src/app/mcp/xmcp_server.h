/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_server.h - MCP (Model Context Protocol) server
 *
 * KEY CONCEPT:
 *   Exposes Xray language tools to AI assistants via MCP over stdio.
 *   Enables AI to check code, look up syntax, and search stdlib.
 */

#ifndef XMCP_SERVER_H
#define XMCP_SERVER_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stdio.h>

/* Forward declarations */
typedef struct XrJsonValue XrJsonValue;
typedef struct XrayIsolate XrayIsolate;
typedef struct XmcpKnowledge XmcpKnowledge;

/* MCP server state */
typedef struct XmcpServer {
    /* stdio transport */
    char  *read_buf;        /* Incremental read buffer */
    size_t read_cap;
    size_t read_len;

    /* Parser isolate (for xray_check tool) */
    XrayIsolate *isolate;

    /* Knowledge base (syntax spec + stdlib index) */
    XmcpKnowledge *knowledge;

    /* Logging */
    FILE *log_file;         /* NULL = stderr only */
    int   log_level;        /* 0=error, 1=warn, 2=info, 3=debug */

    /* Server lifecycle */
    bool initialized;
    bool shutdown;
} XmcpServer;

/* Create a new MCP server. Returns NULL on allocation failure. */
XR_FUNC XmcpServer *xmcp_server_new(void);

/* Destroy server and free all resources. */
XR_FUNC void xmcp_server_free(XmcpServer *server);

/* Run the server main loop (blocking, reads stdin, writes stdout).
 * Returns 0 on clean shutdown, non-zero on error. */
XR_FUNC int xmcp_server_run(XmcpServer *server);

#endif // XMCP_SERVER_H
