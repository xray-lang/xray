/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mcp_service.c - Process fixture for the actual MCP stdio service
 */
#include "app/mcp/xmcp_server.h"
#include <string.h>
int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "mcp-server") || argc > 3) return 2;
    XmcpServerOptions options; xmcp_server_options_default(&options);
    if (argc == 3) {
        if (strcmp(argv[2], "--enable-runner")) return 2;
        options.enable_runner = true;
    }
    XmcpServer *server = xmcp_server_new(&options);
    if (!server) return 1;
    int result = xmcp_server_run(server);
    xmcp_server_free(server); return result;
}
