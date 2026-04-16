/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli.h - Xray command line interface
 *
 * KEY CONCEPT:
 *   CLI entry design:
 *   - xray [options]                     Enter REPL
 *   - xray [options] <file.xr> [args...] Run script
 *   - xray build [opts] <file.xr> [-o]   Compile to binary (AOT only)
 */

#ifndef XCLI_H
#define XCLI_H

#include "../../base/xdefs.h"

// cmd_run - Run Xray program
// Usage: xray [options] <file.xr> [script args...]
// Returns 0 on success, non-zero on failure
XR_FUNC int cmd_run(int argc, char **argv);

// cmd_repl - Start interactive REPL environment
// Usage: xray [options]
// Returns 0 on success, non-zero on failure
XR_FUNC int cmd_repl(int argc, char **argv);

// cmd_test - Run test suite
// Usage: xray test [test options]
// Returns 0 on success, non-zero on failure
XR_FUNC int cmd_test(int argc, char **argv);

// cmd_pkg - Package management command
// Usage: xray pkg <subcommand> [options]
// Subcommands: init, add, remove, install, update, tree, login, publish
// Returns 0 on success, non-zero on failure
XR_FUNC int cmd_pkg(int argc, char **argv);

// cmd_check - Syntax check command
// Usage: xray check [options] <file or directory...>
// Checks source files for syntax errors without execution.
// Returns 0 if no errors, non-zero if errors found
XR_FUNC int cmd_check(int argc, char **argv);

// cmd_fmt - Code formatting command
// Usage: xray fmt [options] <file or directory...>
// Formats Xray source code with consistent style.
// Returns 0 on success, non-zero on failure
XR_FUNC int cmd_fmt(int argc, char **argv);

// cmd_compile - Compile to bytecode
// Usage: xray compile [options] <file.xr> -o <output>
// Compiles to bytecode (.xrc) or C source (.c/.h)
// Output format auto-detected from extension or via --format
// Returns 0 on success, non-zero on failure
XR_FUNC int cmd_compile(int argc, char **argv);

// cmd_build - Compile to executable
// Usage: xray build [options] <file.xr> -o <output>
// Compiles script to bytecode, embeds boot code, links xray runtime
// Returns 0 on success, non-zero on failure
XR_FUNC int cmd_build(int argc, char **argv);

// cmd_deps - Analyze project dependencies
// Usage: xray deps [options] <file.xr>
// Analyzes dependencies and generates install scripts
// Returns 0 on success, non-zero on failure
XR_FUNC int cmd_deps(int argc, char **argv);

#ifdef XR_HAS_LSP
// cmd_lsp - Start LSP server
// Usage: xray lsp [options]
// Starts Language Server Protocol server for IDE integration
// Returns 0 on success, non-zero on failure
XR_FUNC int cmd_lsp(int argc, char **argv);
#endif

#ifdef XR_HAS_DAP
// cmd_dap - Start DAP server
// Usage: xray dap [options]
// Starts Debug Adapter Protocol server for debugging
// Returns 0 on success, non-zero on failure
XR_FUNC int cmd_dap(int argc, char **argv);
#endif

// print_usage - Print usage help
XR_FUNC void print_usage(void);

// print_version - Print version info
XR_FUNC void print_version(void);

// Per-command help functions (used by 'xray help <cmd>')
XR_FUNC void print_run_help(void);
XR_FUNC void print_repl_help(void);
XR_FUNC void print_test_help(void);
XR_FUNC void print_check_help(void);
XR_FUNC void print_fmt_help(void);
XR_FUNC void print_compile_help(void);
XR_FUNC void print_build_help(void);
XR_FUNC void print_deps_help(void);
XR_FUNC void print_pkg_help(void);

#endif // XCLI_H
