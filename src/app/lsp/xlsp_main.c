/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_main.c - LSP server entry point
 */

#include "xlsp_server.h"
#include "xray_version.h"
#include <stdio.h>
#include <string.h>

static void print_usage(const char *prog) {
    fprintf(stderr, "xray Language Server Protocol implementation\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --stdio     Use stdio for communication (default)\n");
    fprintf(stderr, "  --version   Print version and exit\n");
    fprintf(stderr, "  --help      Print this help message\n");
}

static void print_version(void) {
    fprintf(stderr, "xray-lsp version %s\n", XRAY_VERSION_STRING);
}

int main(int argc, char *argv[]) {
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
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
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        print_usage(argv[0]);
        return 1;
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
