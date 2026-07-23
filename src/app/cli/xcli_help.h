/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_help.h - Auto-generated help text from command specs
 *
 * KEY CONCEPT:
 *   All help text derives from XrCliCommandSpec. No hand-written help.
 *   `xray --help`, `xray help <cmd>`, and `xray <cmd> --help` all
 *   use the same spec-driven generation path.
 */

#ifndef XCLI_HELP_H
#define XCLI_HELP_H

#include "xcli_spec.h"
#include <stdbool.h>
#include <stdio.h>

/* Print top-level usage (xray --help). */
XR_FUNC void xr_cli_print_usage(void);

/* Print the human version or the release-identity JSON contract. */
XR_FUNC void xr_cli_print_version(bool json);

/* Write the build identity object without a trailing newline. */
XR_FUNC void xr_cli_print_build_identity_json(FILE *out, int indent);

/* Print help for a specific command (xray help <cmd> / xray <cmd> --help). */
XR_FUNC void xr_cli_print_command_help(const XrCliCommandSpec *spec);

/* Print help for a subcommand parent (e.g. xray pkg --help). */
XR_FUNC void xr_cli_print_subcommand_help(const XrCliCommandSpec *parent);

#endif  // XCLI_HELP_H
