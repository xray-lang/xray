/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_dap.c - DAP debug server subcommand
 *
 * Usage:
 *   xray dap                    # stdio transport (for IDE extension)
 *   xray dap --port <port>      # TCP server on port (for remote debugging)
 *   xray dap --port 0           # TCP server on random port
 */

#ifdef XR_HAS_DAP

#include "xcli.h"
#include "xcli_utils.h"
#include "../dap/xdap_controller.h"
#include "../dap/xdap_transport.h"
#include "../dap/xdap_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_dap_usage(void) {
    fprintf(stderr, "xray Debug Adapter Protocol implementation\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage: xray dap [options]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --port <port>   Start TCP server on port (0 for random)\n");
    fprintf(stderr, "  --help          Show this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Without options, uses stdio transport for IDE extension.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Remote debugging example:\n");
    fprintf(stderr, "  xray dap --port 4711\n");
    fprintf(stderr, "  Then configure your debugger to connect to localhost:4711\n");
}

int cmd_dap(int argc, char **argv) {
    int tcp_port = -1;  // -1 means use stdio
    
    // Parse arguments
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --port requires a port number\n");
                return 1;
            }
            if (!cli_parse_port(argv[++i], &tcp_port)) {
                fprintf(stderr, "Error: invalid port number '%s'\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_dap_usage();
            return 0;
        }
    }
    
    // Create transport
    XdapTransport *transport;
    
    if (tcp_port >= 0) {
        // TCP transport for remote debugging
        transport = xdap_transport_tcp_server(tcp_port);
        if (!transport) {
            fprintf(stderr, "Failed to create TCP transport on port %d\n", tcp_port);
            return 1;
        }
    } else {
        // stdio transport for IDE extension
        transport = xdap_transport_stdio();
        if (!transport) {
            fprintf(stderr, "Failed to create stdio transport\n");
            return 1;
        }
    }
    
    // Create controller
    XdapController *ctrl = xdap_controller_new(transport);
    if (!ctrl) {
        fprintf(stderr, "Failed to create DAP controller\n");
        xdap_transport_free(transport);
        return 1;
    }
    
    // Run main loop
    int result = xdap_run(ctrl);
    
    // Cleanup
    xdap_controller_free(ctrl);
    
    return result;
}

#endif // XR_HAS_DAP
