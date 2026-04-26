/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_dispatch.h - Top-level CLI dispatch and command suggestion
 *
 * KEY CONCEPT:
 *   Single entry point that routes argv to the correct command handler.
 *   Handles: zero-arg REPL, global flags, command routing,
 *   .xr script shortcut, and "did you mean?" suggestions.
 */

#ifndef XCLI_DISPATCH_H
#define XCLI_DISPATCH_H

#include "xcli_spec.h"

/* Register all command handlers into the spec table.
 * Must be called once before xr_cli_main(). */
void xr_cli_register_all_handlers(void);

/* Main CLI entry point. Called from main() after signal setup.
 * Returns an exit code (0 on success). */
XR_FUNC int xr_cli_main(int argc, char **argv);

/* Suggest a similar command name (Levenshtein distance <= 2).
 * Prints suggestion to stderr. Returns true if a suggestion was printed. */
XR_FUNC bool xr_cli_suggest_command(const char *input);

#endif  // XCLI_DISPATCH_H
