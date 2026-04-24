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
#include "xcli_spec.h"
#include "xcli_fs.h"
#include "../../base/xchecks.h"
#include "../dap/xdap_controller.h"
#include "../dap/xdap_transport.h"
#include "../dap/xdap_protocol.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

XR_FUNC int cmd_dap(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");

    int tcp_port = -1;  /* -1 means use stdio */
    bool port_set = xr_cli_opt_present(&inv->options, "port");

    if (port_set) {
        tcp_port = xr_cli_opt_int(&inv->options, "port", -1);
    }

    /* Env var fallback: XRAY_DAP_PORT only kicks in when --port was not
     * passed. Same tiered precedence as XRAY_LSP_LOG. */
    if (!port_set) {
        const char *env_port = getenv("XRAY_DAP_PORT");
        if (env_port && env_port[0] != '\0') {
            int env_tcp_port;
            if (!xr_cli_parse_port(env_port, &env_tcp_port)) {
                xr_cli_error("dap", "invalid XRAY_DAP_PORT='%s'", env_port);
                return XR_CLI_EXIT_FAIL;
            }
            tcp_port = env_tcp_port;
        }
    }

    /* Create transport */
    XdapTransport *transport;

    if (tcp_port >= 0) {
        transport = xdap_transport_tcp_server(tcp_port);
        if (!transport) {
            xr_cli_error("dap", "failed to create TCP transport on port %d", tcp_port);
            return XR_CLI_EXIT_INTERNAL;
        }
    } else {
        transport = xdap_transport_stdio();
        if (!transport) {
            xr_cli_error("dap", "failed to create stdio transport");
            return XR_CLI_EXIT_INTERNAL;
        }
    }

    /* Create controller */
    XdapController *ctrl = xdap_controller_new(transport);
    if (!ctrl) {
        xr_cli_error("dap", "failed to create DAP controller");
        xdap_transport_free(transport);
        return XR_CLI_EXIT_INTERNAL;
    }

    /* Run main loop */
    int result = xdap_run(ctrl);

    xdap_controller_free(ctrl);

    return (result != 0) ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}

#endif // XR_HAS_DAP
