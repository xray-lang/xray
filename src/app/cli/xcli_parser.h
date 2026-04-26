/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_parser.h - Unified CLI argument parser
 *
 * KEY CONCEPT:
 *   Parses argc/argv against an XrCliCommandSpec into an XrCliInvocation.
 *   No command handler should call getopt or manually scan argv.
 */

#ifndef XCLI_PARSER_H
#define XCLI_PARSER_H

#include "xcli_spec.h"

/* Parse global flags from the beginning of argv.
 * Fills ctx->color, ctx->verbose, etc. from --color/--no-color/--verbose/etc.
 * Returns the number of argv entries consumed (to skip before command routing).
 * If --help or --version is encountered, returns a negative sentinel:
 *   -1 = --help requested
 *   -2 = --version requested
 */
XR_FUNC int xr_cli_parse_global(int argc, char **argv, XrCliContext *ctx);

/* Parse command-specific options + positionals from argv into an invocation.
 * argv[0] should be the first argument AFTER the command name.
 * argc is the count of those arguments.
 * Returns XR_CLI_EXIT_OK on success, XR_CLI_EXIT_USAGE on parse error.
 * On error, diagnostics are printed to stderr via xcli_diag.
 */
XR_FUNC XrCliExitCode xr_cli_parse_command(const XrCliCommandSpec *spec, int argc, char **argv,
                                           const XrCliContext *ctx, XrCliInvocation *inv);

/* Free resources allocated by xr_cli_parse_command.
 * Safe to call on a zero-initialized invocation. */
XR_FUNC void xr_cli_invocation_free(XrCliInvocation *inv);

#endif  // XCLI_PARSER_H
