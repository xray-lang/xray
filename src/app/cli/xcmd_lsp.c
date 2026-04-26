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
#include "xcli_spec.h"
#include "../../base/xchecks.h"
#include "../lsp/xlsp_server.h"
#include <stdio.h>

XR_FUNC int cmd_lsp(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    (void) inv; /* --stdio is the only transport, no options to read */

    /* Create and run server */
    XrLspServer *server = xlsp_server_new();
    if (!server) {
        xr_cli_error("lsp", "failed to create LSP server");
        return XR_CLI_EXIT_INTERNAL;
    }

    int result = xlsp_server_run(server);
    xlsp_server_free(server);

    return (result != 0) ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}

#endif  // XR_HAS_LSP
