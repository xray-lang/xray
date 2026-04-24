/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_diag.h - CLI diagnostics and exit codes
 *
 * KEY CONCEPT:
 *   Unified exit code model and diagnostic output for all CLI commands.
 *   All user-visible error messages go through this module.
 */

#ifndef XCLI_DIAG_H
#define XCLI_DIAG_H

#include "../../base/xdefs.h"
#include <stdbool.h>

/* Exit code model:
 *   0 = success
 *   1 = command execution failure (test fail, syntax error, network error)
 *   2 = usage error (bad args, unknown option, missing required param)
 *   3 = feature unavailable (compile-time disabled, unimplemented backend)
 *   4 = internal error (allocation failure, invariant violation)
 */
typedef enum {
    XR_CLI_EXIT_OK          = 0,
    XR_CLI_EXIT_FAIL        = 1,
    XR_CLI_EXIT_USAGE       = 2,
    XR_CLI_EXIT_UNAVAILABLE = 3,
    XR_CLI_EXIT_INTERNAL    = 4,
} XrCliExitCode;

/* Print a CLI error prefixed with the program/command name.
 * Format: "xray <cmd>: <message>\n"  or  "xray: <message>\n" if cmd is NULL.
 */
XR_FUNC void xr_cli_error(const char *cmd, const char *fmt, ...)
    XR_PRINTF_FMT(2, 3);

/* Print a CLI warning (same format, to stderr). */
XR_FUNC void xr_cli_warn(const char *cmd, const char *fmt, ...)
    XR_PRINTF_FMT(2, 3);

/* Print "unknown command" with optional suggestion. */
XR_FUNC void xr_cli_unknown_command(const char *input);

/* Print "unknown option" for a command. */
XR_FUNC void xr_cli_unknown_option(const char *cmd, const char *option);

/* Print "missing argument" for a required option. */
XR_FUNC void xr_cli_missing_argument(const char *cmd, const char *option);

#endif // XCLI_DIAG_H
