/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_lsp.c - LSP server subcommand
 *
 * Usage:
 *   xray lsp              # Start LSP server (stdio transport)
 *   xray lsp --stdio      # Same as above (explicit)
 */

#ifdef XR_HAS_LSP

#include "xcli.h"
#include "../lsp/xlsp_server.h"
#include <stdio.h>
#include <string.h>

static void print_lsp_usage(void) {
    fprintf(stderr, "xray Language Server Protocol implementation\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage: xray lsp [options]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --stdio     Use stdio for communication (default)\n");
    fprintf(stderr, "  --version   Print version and exit\n");
    fprintf(stderr, "  --help      Print this help message\n");
}

int cmd_lsp(int argc, char **argv) {
    // Parse command line arguments
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_lsp_usage();
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            print_version();
            return 0;
        }
        if (strcmp(argv[i], "--stdio") == 0) {
            // Default behavior, ignored
            continue;
        }
    }
    
    // Create and run server
    XrLspServer *server = xlsp_server_new();
    if (!server) {
        fprintf(stderr, "Failed to create LSP server\n");
        return 1;
    }
    
    int result = xlsp_server_run(server);
    
    xlsp_server_free(server);
    
    return result;
}

#endif // XR_HAS_LSP
