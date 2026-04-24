/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_diag.c - CLI diagnostics and exit codes
 *
 * KEY CONCEPT:
 *   All user-visible CLI error/warning messages are routed through
 *   this module to ensure consistent formatting and exit code semantics.
 */

#include "xcli_diag.h"
#include <stdarg.h>
#include <stdio.h>

void xr_cli_error(const char *cmd, const char *fmt, ...) {
    if (cmd) {
        fprintf(stderr, "xray %s: ", cmd);
    } else {
        fprintf(stderr, "xray: ");
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void xr_cli_warn(const char *cmd, const char *fmt, ...) {
    if (cmd) {
        fprintf(stderr, "xray %s: warning: ", cmd);
    } else {
        fprintf(stderr, "xray: warning: ");
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void xr_cli_unknown_command(const char *input) {
    fprintf(stderr, "xray: unknown command '%s'\n", input);
    fprintf(stderr, "\nRun 'xray --help' to see all commands.\n");
}

void xr_cli_unknown_option(const char *cmd, const char *option) {
    xr_cli_error(cmd, "unknown option '%s'", option);
    if (cmd) {
        fprintf(stderr, "Run 'xray %s --help' for usage.\n", cmd);
    }
}

void xr_cli_missing_argument(const char *cmd, const char *option) {
    xr_cli_error(cmd, "option '%s' requires an argument", option);
}
