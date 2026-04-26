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
 *   Table-driven method dispatch with signal-safe shutdown.
 */

#ifndef XMCP_SERVER_H
#define XMCP_SERVER_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stdio.h>
#include <signal.h>

/* Forward declarations */
typedef struct XrJsonValue XrJsonValue;
typedef struct XrayIsolate XrayIsolate;
typedef struct XmcpKnowledge XmcpKnowledge;

/* MCP server state */
typedef struct XmcpServer {
    /* stdio transport */
    char *read_buf; /* Incremental read buffer */
    size_t read_cap;
    size_t read_len;

    /* Parser isolate (for xray_check tool) */
    XrayIsolate *isolate;

    /* Knowledge base (syntax spec + stdlib index) */
    XmcpKnowledge *knowledge;

    /* Logging */
    FILE *log_file; /* NULL = stderr only */
    int log_level;  /* 0=error, 1=warn, 2=info, 3=debug */

    /* Feature flags (for dynamic capability inference) */
    bool has_tools;     /* true if tools are registered */
    bool has_resources; /* true if resources are registered */
    bool has_prompts;   /* true if prompts are registered */

    /* Per-request progress token (-1 = no progress tracking) */
    int64_t current_progress_token;

    /* Server lifecycle */
    bool initialized;
    volatile sig_atomic_t shutdown; /* signal-safe shutdown flag */
} XmcpServer;

/* Create a new MCP server. Returns NULL on allocation failure. */
XR_FUNC XmcpServer *xmcp_server_new(void);

/* Destroy server and free all resources. */
XR_FUNC void xmcp_server_free(XmcpServer *server);

/* Run the server main loop (blocking, reads stdin, writes stdout).
 * Returns 0 on clean shutdown, non-zero on error. */
XR_FUNC int xmcp_server_run(XmcpServer *server);

/* Write a JSON-RPC message to stdout (used by notification infrastructure). */
XR_FUNC void xmcp_write_message(const char *json, size_t len);

#endif  // XMCP_SERVER_H
