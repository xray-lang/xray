/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli.c - Xray command line main entry
 *
 * KEY CONCEPT:
 *   Unified CLI interface, routes to subcommands:
 *   - xray                       Enter REPL
 *   - xray <file.xr>             Run script
 *   - xray <cmd> [opts] [args]   Subcommand mode
 */

#include "xcli.h"
#include "xcli_utils.h"
#include "xray.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#if defined(__APPLE__) || (defined(__linux__) && !defined(__ANDROID__))
#include <execinfo.h>
#define HAS_BACKTRACE 1
#endif

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
    !(defined(__has_feature) && (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)))
static void crash_handler(int sig) {
    // Only use async-signal-safe functions: write(), _exit()
    const char *msg = "\n=== CRASH: signal unknown ===\n";
    if (sig == SIGSEGV) msg = "\n=== CRASH: SIGSEGV ===\n";
#ifdef SIGBUS
    else if (sig == SIGBUS) msg = "\n=== CRASH: SIGBUS ===\n";
#endif
    write(STDERR_FILENO, msg, strlen(msg));
#ifdef HAS_BACKTRACE
    void *bt[32];
    int n = backtrace(bt, 32);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
#endif
    _exit(128 + sig);
}
#endif

typedef int (*CmdHandler)(int argc, char **argv);

typedef void (*CmdHelpFn)(void);

typedef struct {
    const char *name;       // Command name
    CmdHandler handler;     // Handler function
    int arg_offset;         // Arg offset (1=skip cmd, 2=skip cmd+subcmd)
    bool is_pkg_alias;      // Is package management alias
    CmdHelpFn help_fn;      // Per-command help printer (NULL = none)
} CmdEntry;

// Forward declarations
static void print_cmd_help(const char *cmd);
static int suggest_command(const char *input);
static int cmd_help(int argc, char **argv);
static int cmd_info(int argc, char **argv);

// Command table - all commands unified here
static const CmdEntry commands[] = {
    // Core commands
    {"run",      cmd_run,     1, false, print_run_help},
    {"repl",     cmd_repl,    2, false, print_repl_help},
    {"test",     cmd_test,    1, false, print_test_help},
    {"check",    cmd_check,   2, false, print_check_help},
    {"fmt",      cmd_fmt,     2, false, print_fmt_help},
    {"compile",  cmd_compile, 1, false, print_compile_help},
    {"build",    cmd_build,   1, false, print_build_help},
    {"deps",     cmd_deps,    1, false, print_deps_help},
    {"pkg",      cmd_pkg,     2, false, print_pkg_help},
    // Utility commands
    {"help",     cmd_help,    2, false, NULL},
    {"info",     cmd_info,    2, false, NULL},
    // IDE integration (optional modules)
#ifdef XR_HAS_LSP
    {"lsp",      cmd_lsp,     2, false, NULL},
#endif
#ifdef XR_HAS_DAP
    {"dap",      cmd_dap,     2, false, NULL},
#endif
#ifdef XR_HAS_MCP
    {"mcp-server", cmd_mcp_server, 2, false, NULL},
#endif
    // Package management aliases (xray init = xray pkg init)
    {"init",     cmd_pkg,    1, true, print_pkg_help},
    {"add",      cmd_pkg,    1, true, print_pkg_help},
    {"remove",   cmd_pkg,    1, true, print_pkg_help},
    {"install",  cmd_pkg,    1, true, print_pkg_help},
    {"update",   cmd_pkg,    1, true, print_pkg_help},
    {"tree",     cmd_pkg,    1, true, print_pkg_help},
    {"publish",  cmd_pkg,    1, true, print_pkg_help},
    {"login",    cmd_pkg,    1, true, print_pkg_help},
    // End marker
    {NULL, NULL, 0, false, NULL}
};

int main(int argc, char **argv) {
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
    !(defined(__has_feature) && (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)))
    signal(SIGSEGV, crash_handler);
#ifdef SIGBUS
    signal(SIGBUS, crash_handler);
#endif
#endif
    // No arguments: start REPL
    if (argc < 2) {
        return cmd_repl(0, NULL);
    }

    const char *cmd = argv[1];

    // Global options
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        print_version();
        return 0;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage();
        return 0;
    }

    // Table-driven command routing
    for (int i = 0; commands[i].name != NULL; i++) {
        if (strcmp(cmd, commands[i].name) == 0) {
            int offset = commands[i].arg_offset;
            return commands[i].handler(argc - offset, argv + offset);
        }
    }

    // Options starting with -, pass to cmd_run
    if (cmd[0] == '-') {
        return cmd_run(argc, argv);
    }

    // Script file: run directly
    // Check if .xr file or existing file
    const char *ext = strrchr(cmd, '.');
    if (ext && strcmp(ext, ".xr") == 0) {
        return cmd_run(argc, argv);
    }

    // Try running as script file (may have no extension)
    if (cli_file_exists(cmd)) {
        return cmd_run(argc, argv);
    }

    // Unknown command: try to suggest
    if (!suggest_command(cmd)) {
        fprintf(stderr, "Error: unknown command '%s'\n", cmd);
        fprintf(stderr, "\n");
        fprintf(stderr, "Run 'xray --help' to see all commands\n");
    }
    return 1;
}

// Command suggestion - uses commands[] table directly
static int suggest_command(const char *input) {
    const char *best = NULL;
    int best_dist = 999;

    for (int i = 0; commands[i].name != NULL; i++) {
        int dist = cli_string_distance(input, commands[i].name);
        if (dist < best_dist && dist <= 2) {
            best_dist = dist;
            best = commands[i].name;
        }
    }

    if (best) {
        fprintf(stderr, "Error: unknown command '%s'\n", input);
        fprintf(stderr, "\nDid you mean?\n");
        fprintf(stderr, "  xray %s\n\n", best);
        return 1;
    }
    return 0;
}

// help command handler
static int cmd_help(int argc, char **argv) {
    if (argc >= 1) {
        print_cmd_help(argv[0]);
    } else {
        print_usage();
    }
    return 0;
}

// info command handler
static int cmd_info(int argc, char **argv) {
    (void)argc;
    (void)argv;
    print_version();
    printf("\n");
    printf("Environment:\n");
    const char *home = getenv("HOME");
    if (home) {
        printf("  Package cache: %s/.xray/packages/\n", home);
    }
    printf("  Config file: xray.toml\n");
    printf("  Registry: pkg.xray-lang.org\n");
    return 0;
}

// Subcommand help - table-driven lookup via help_fn pointer
static void print_cmd_help(const char *cmd) {
    for (int i = 0; commands[i].name != NULL; i++) {
        if (strcmp(cmd, commands[i].name) == 0) {
            if (commands[i].help_fn) {
                commands[i].help_fn();
            } else {
                // Commands without dedicated help (lsp, dap, etc.)
                // invoke handler with --help
                char *help_argv[] = {"--help"};
                commands[i].handler(1, help_argv);
            }
            return;
        }
    }
    printf("Unknown command: %s\n", cmd);
    printf("Run 'xray --help' to see all commands\n");
}

void print_version(void) {
    printf("xray v%d.%d.%d (", XRAY_VERSION_MAJOR, XRAY_VERSION_MINOR, XRAY_VERSION_PATCH);
#ifdef XRAY_HAS_JIT
    printf("JIT");
#elif defined(XRAY_HAS_AOT)
    printf("AOT");
#else
    printf("VM");
#endif

    // Print architecture
#if defined(__x86_64__) || defined(_M_X64)
    printf(", x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    printf(", arm64");
#elif defined(__i386__) || defined(_M_IX86)
    printf(", x86");
#endif

    // Print OS
#if defined(__linux__)
    printf("-linux");
#elif defined(__APPLE__)
    printf("-darwin");
#elif defined(_WIN32)
    printf("-windows");
#endif

    printf(")\n");
}

void print_usage(void) {
    printf("Xray - High-performance scripting language\n");
    printf("\n");
    printf("Usage:\n");
    printf("  xray [options] [file] [args...]     Run script\n");
    printf("  xray <command> [options] [args...]  Execute command\n");
    printf("\n");
    printf("Commands:\n");
    printf("  run         Run script or project\n");
    printf("  repl        Enter interactive environment\n");
    printf("  test        Run tests\n");
    printf("  init        Initialize new project\n");
    printf("  add         Add dependency\n");
    printf("  remove      Remove dependency\n");
    printf("  install     Install all dependencies\n");
    printf("  update      Update dependencies\n");
    printf("  tree        Show dependency tree\n");
    printf("  build       Compile to binary\n");
    printf("  compile     Compile to bytecode\n");
    printf("  check       Syntax check\n");
    printf("  fmt         Format code\n");
#ifdef XR_HAS_LSP
    printf("  lsp         Start LSP server\n");
#endif
#ifdef XR_HAS_DAP
    printf("  dap         Start DAP debug server\n");
#endif
#ifdef XR_HAS_MCP
    printf("  mcp-server  Start MCP server (AI assistant)\n");
#endif
    printf("  info        Environment info\n");
    printf("\n");
    printf("Options:\n");
    printf("  -v, --version    Show version\n");
    printf("  -h, --help       Show help\n");
    printf("  -e '<code>'      Execute code string\n");
    printf("\n");
    printf("Examples:\n");
    printf("  xray                    Enter REPL\n");
    printf("  xray app.xr             Run script\n");
    printf("  xray init               Initialize project\n");
    printf("  xray add xray/redis     Add dependency\n");
    printf("  xray build app.xr       Compile to binary\n");
    printf("\n");
    printf("Run 'xray help <command>' for command details.\n");
}
