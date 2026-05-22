/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_mcp_server.c - CLI entry for `xray mcp-server`
 *
 * KEY CONCEPT:
 *   The CLI owns the option-parsing surface and lifecycle of the MCP
 *   subcommand.  src/app/mcp/ provides the protocol implementation and stays
 *   free of any CLI-specific includes; the dependency is one-directional
 *   (cli -> mcp), avoiding a sibling-app cycle.
 */

#ifdef XR_HAS_MCP

#include "xcli.h"
#include "xcli_spec.h"
#include "xcli_diag.h"
#include "../mcp/xmcp_server.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>

static int parse_log_level(const char *level) {
    if (!level)
        return 2; /* info */
    if (strcmp(level, "error") == 0)
        return 0;
    if (strcmp(level, "warn") == 0)
        return 1;
    if (strcmp(level, "info") == 0)
        return 2;
    if (strcmp(level, "debug") == 0)
        return 3;
    return 2; /* default: info */
}

XR_FUNC int cmd_mcp_server(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");

    const char *level_str = xr_cli_opt_string(&inv->options, "log-level", NULL);
    const char *log_file_path = xr_cli_opt_string(&inv->options, "log-file", NULL);
    int log_level = parse_log_level(level_str);

    XmcpServerOptions options;
    xmcp_server_options_default(&options);
    options.enable_runner = xr_cli_opt_bool(&inv->options, "enable-runner");

    XmcpServer *s = xmcp_server_new(&options);
    if (!s) {
        xr_cli_error("mcp-server", "failed to create MCP server");
        return XR_CLI_EXIT_INTERNAL;
    }
    s->log_level = log_level;

    if (log_file_path) {
        s->log_file = fopen(log_file_path, "a");
        if (!s->log_file) {
            xr_cli_warn("mcp-server", "cannot open log file '%s'", log_file_path);
        }
    }

    int rc = xmcp_server_run(s);
    xmcp_server_free(s);
    return (rc != 0) ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}

#endif /* XR_HAS_MCP */
