/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_spec.c - Command/option specs, option map accessors, command registry
 *
 * KEY CONCEPT:
 *   Single source of truth for all CLI commands and their options.
 *   Every command is a static spec; help text, parsing, and dispatch
 *   all derive from these specs.
 */

#include "xcli_spec.h"
#include "../../base/xchecks.h"
#include <string.h>

/* ========== Option Specs per Command ========== */

static const XrCliOptionSpec run_options[] = {
    {"trace", 't', XR_CLI_VALUE_NONE, false, false, NULL, "Trace execution"},
    {"dump-bytecode", 'd', XR_CLI_VALUE_NONE, false, false, NULL, "Dump bytecode"},
    {"workers", 'W', XR_CLI_VALUE_INT, false, false, "N", "Number of worker threads"},
    {"coro-watch", 'w', XR_CLI_VALUE_INT, false, false, "MS", "Coroutine watch interval (ms)"},
    {"coro-http", 'H', XR_CLI_VALUE_INT, false, false, "PORT", "Coroutine HTTP monitor port"},
    {"no-jit", 'J', XR_CLI_VALUE_NONE, false, false, NULL, "Disable JIT compiler"},
    {"jit-force", 'F', XR_CLI_VALUE_NONE, false, false, NULL, "Force JIT on first call"},
    {"jit-stats", 'S', XR_CLI_VALUE_NONE, false, false, NULL, "Print JIT statistics"},
    {"dump-ic", 'I', XR_CLI_VALUE_NONE, false, false, NULL, "Dump inline cache feedback"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec eval_options[] = {XR_CLI_OPT_END};

static const XrCliOptionSpec repl_options[] = {
    {"no-color", 'n', XR_CLI_VALUE_NONE, false, false, NULL, "Disable color output"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec test_options[] = {
    {"verbose", 'v', XR_CLI_VALUE_NONE, false, false, NULL, "Verbose output"},
    {"fail-fast", 'F', XR_CLI_VALUE_NONE, false, false, NULL, "Stop on first failure"},
    {"filter", 'f', XR_CLI_VALUE_STRING, false, false, "PATTERN", "Only run matching tests"},
    {"no-jit", 'J', XR_CLI_VALUE_NONE, false, false, NULL, "Disable JIT compiler"},
    {"jit-force", 0, XR_CLI_VALUE_NONE, false, false, NULL, "Force JIT on first call"},
    {"quiet", 'q', XR_CLI_VALUE_NONE, false, false, NULL, "Quiet mode (exit code only)"},
    {"jobs", 'j', XR_CLI_VALUE_INT, false, false, "N", "Parallel threads (default 1)"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec check_options[] = {
    {"verbose", 'v', XR_CLI_VALUE_NONE, false, false, NULL, "Show all checked files"},
    {"quiet", 'q', XR_CLI_VALUE_NONE, false, false, NULL, "Show errors only"},
    {"strict", 's', XR_CLI_VALUE_NONE, false, false, NULL, "Enable type checking"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec fmt_options[] = {
    {"check", 'c', XR_CLI_VALUE_NONE, false, false, NULL, "Check only, do not modify"},
    {"verbose", 'v', XR_CLI_VALUE_NONE, false, false, NULL, "Show all processed files"},
    {"tabs", 't', XR_CLI_VALUE_NONE, false, false, NULL, "Use tab indent"},
    {"indent", 'i', XR_CLI_VALUE_INT, false, false, "N", "Indent spaces (default 4)"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec compile_options[] = {
    {"output", 'o', XR_CLI_VALUE_STRING, false, false, "FILE", "Output file path"},
    {"format", 'f', XR_CLI_VALUE_STRING, false, false, "FMT", "Output format: bytecode, c, header"},
    {"strip-debug", 's', XR_CLI_VALUE_NONE, false, false, NULL, "Remove debug info"},
    {"strip-source", 'S', XR_CLI_VALUE_NONE, false, false, NULL, "Remove source file path"},
    {"name", 'n', XR_CLI_VALUE_STRING, false, false, "NAME", "C variable name prefix"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec build_options[] = {
    {"output", 'o', XR_CLI_VALUE_STRING, false, false, "FILE", "Output file path"},
    {"c-only", 'c', XR_CLI_VALUE_NONE, false, false, NULL, "Output C source only"},
    {"cc", 'C', XR_CLI_VALUE_STRING, false, false, "CC", "C compiler to use"},
    {"opt", 'O', XR_CLI_VALUE_STRING, false, false, "LEVEL", "Optimization (0,1,2,3,s,fast)"},
    {"sysroot", 'r', XR_CLI_VALUE_STRING, false, false, "DIR", "System root directory"},
    {"strip", 'S', XR_CLI_VALUE_NONE, false, false, NULL, "Strip debug symbols"},
    {"native", 'N', XR_CLI_VALUE_NONE, false, false, NULL, "Use AOT native backend"},
    {"xi", 0, XR_CLI_VALUE_NONE, false, false, NULL, "Use Xi IR pipeline (with --native)"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec deps_options[] = {
    {"output", 'o', XR_CLI_VALUE_STRING, false, false, "FILE", "Output file path"},
    {"shell", 's', XR_CLI_VALUE_NONE, false, false, NULL, "Shell script format"},
    {"json", 'j', XR_CLI_VALUE_NONE, false, false, NULL, "JSON format"},
    {"list", 'l', XR_CLI_VALUE_NONE, false, false, NULL, "Simple list format"},
    XR_CLI_OPT_END};

static const XrCliOptionSpec pkg_options[] = {XR_CLI_OPT_END};

static const XrCliOptionSpec empty_options[] = {XR_CLI_OPT_END};

#ifdef XR_HAS_LSP
static const XrCliOptionSpec lsp_options[] = {
    {"stdio", 0, XR_CLI_VALUE_NONE, false, false, NULL, "Use stdio transport (default)"},
    XR_CLI_OPT_END};
#endif

#ifdef XR_HAS_DAP
static const XrCliOptionSpec dap_options[] = {{"port", 'p', XR_CLI_VALUE_INT, false, false, "PORT",
                                               "TCP port (0 for random, omit for stdio)"},
                                              XR_CLI_OPT_END};
#endif

#ifdef XR_HAS_MCP
static const XrCliOptionSpec mcp_options[] = {
    {"log-level", 'l', XR_CLI_VALUE_STRING, false, false, "LEVEL",
     "Log level: error,warn,info,debug"},
    {"log-file", 'f', XR_CLI_VALUE_STRING, false, false, "PATH", "Log to file"},
    XR_CLI_OPT_END};
#endif

/* ========== pkg Subcommands ========== */

static const XrCliCommandSpec pkg_subcommands[] = {
    {"init", "Initialize new project", NULL, empty_options, 0, 0, false, false, NULL, NULL, 0},
    {"add", "Add dependency", NULL, empty_options, 1, -1, false, false, NULL, NULL, 0},
    {"remove", "Remove dependency", NULL, empty_options, 1, -1, false, false, NULL, NULL, 0},
    {"install", "Install all dependencies", NULL, empty_options, 0, 0, false, false, NULL, NULL, 0},
    {"update", "Update dependencies", NULL, empty_options, 0, 0, false, false, NULL, NULL, 0},
    {"tree", "Show dependency tree", NULL, empty_options, 0, 0, false, false, NULL, NULL, 0},
    {"login", "Login to registry", NULL, empty_options, 0, 0, false, false, NULL, NULL, 0},
    {"publish", "Publish package", NULL, empty_options, 0, 0, false, false, NULL, NULL, 0},
    {NULL, NULL, NULL, NULL, 0, 0, false, false, NULL, NULL, 0}};

/* ========== Top-level Command Table ========== */

static XrCliCommandSpec cli_commands[] = {
    /* Execution commands */
    {"run", "Run script or project", NULL, run_options, 0, -1, true, false, NULL, NULL, 0},
    {"eval", "Execute code string", NULL, eval_options, 1, 1, false, false, NULL, NULL, 0},
    {"repl", "Interactive environment", NULL, repl_options, 0, 0, false, false, NULL, NULL, 0},
    {"test", "Run tests", NULL, test_options, 0, -1, false, false, NULL, NULL, 0},
    {"check", "Syntax check", NULL, check_options, 0, -1, false, false, NULL, NULL, 0},
    {"fmt", "Format source code", NULL, fmt_options, 0, -1, false, false, NULL, NULL, 0},

    /* Artifact commands */
    {"compile", "Compile to bytecode or C", NULL, compile_options, 1, 1, false, false, NULL, NULL,
     0},
    {"build", "Compile to binary", NULL, build_options, 1, 1, false, false, NULL, NULL, 0},
    {"deps", "Analyze dependencies", NULL, deps_options, 1, 1, false, false, NULL, NULL, 0},

    /* Package management (has subcommands) */
    {"pkg", "Package management", NULL, pkg_options, 0, -1, false, false, NULL, pkg_subcommands, 8},

    /* Utility commands */
    {"info", "Environment info", NULL, empty_options, 0, 0, false, false, NULL, NULL, 0},
    {"help", "Show help for a command", NULL, empty_options, 0, 1, false, false, NULL, NULL, 0},

/* IDE integration (conditional compilation) */
#ifdef XR_HAS_LSP
    {"lsp", "Start LSP server", NULL, lsp_options, 0, 0, false, false, NULL, NULL, 0},
#endif
#ifdef XR_HAS_DAP
    {"dap", "Start DAP debug server", NULL, dap_options, 0, 0, false, false, NULL, NULL, 0},
#endif
#ifdef XR_HAS_MCP
    {"mcp-server", "Start MCP server", NULL, mcp_options, 0, 0, false, false, NULL, NULL, 0},
#endif

    /* Sentinel */
    {NULL, NULL, NULL, NULL, 0, 0, false, false, NULL, NULL, 0}};

/* ========== Handler Registration ========== */

void xr_cli_register_handler(const char *name, XrCliHandler handler) {
    XR_DCHECK(name != NULL, "name is NULL");
    XR_DCHECK(handler != NULL, "handler is NULL");
    for (int i = 0; cli_commands[i].name != NULL; i++) {
        if (strcmp(cli_commands[i].name, name) == 0) {
            cli_commands[i].handler = handler;
            return;
        }
    }
    XR_DCHECK(false, "unknown command for registration");
}

/* ========== Option Map Accessors ========== */

/* Find option index by long_name. Returns -1 if not found. */
static int find_option_index(const XrCliOptionMap *map, const char *name) {
    XR_DCHECK(map != NULL, "option map is NULL");
    XR_DCHECK(name != NULL, "option name is NULL");
    for (int i = 0; i < map->count; i++) {
        if (map->spec[i].long_name && strcmp(map->spec[i].long_name, name) == 0) {
            return i;
        }
    }
    return -1;
}

bool xr_cli_opt_present(const XrCliOptionMap *map, const char *name) {
    int idx = find_option_index(map, name);
    if (idx < 0)
        return false;
    return map->present[idx];
}

const char *xr_cli_opt_string(const XrCliOptionMap *map, const char *name,
                              const char *default_val) {
    int idx = find_option_index(map, name);
    if (idx < 0 || !map->present[idx])
        return default_val;
    return map->values[idx] ? map->values[idx] : default_val;
}

int xr_cli_opt_int(const XrCliOptionMap *map, const char *name, int default_val) {
    const char *s = xr_cli_opt_string(map, name, NULL);
    if (!s)
        return default_val;
    /* Simple atoi; real validation is done in parser. */
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0')
        return default_val;
    return (int) v;
}

bool xr_cli_opt_bool(const XrCliOptionMap *map, const char *name) {
    return xr_cli_opt_present(map, name);
}

/* ========== Command Registry ========== */

const XrCliCommandSpec *xr_cli_get_commands(void) {
    return cli_commands;
}

const XrCliCommandSpec *xr_cli_find_command(const char *name) {
    XR_DCHECK(name != NULL, "command name is NULL");
    for (int i = 0; cli_commands[i].name != NULL; i++) {
        if (strcmp(cli_commands[i].name, name) == 0) {
            return &cli_commands[i];
        }
    }
    return NULL;
}

int xr_cli_option_count(const XrCliOptionSpec *opts) {
    if (!opts)
        return 0;
    int n = 0;
    while (opts[n].long_name != NULL) {
        n++;
    }
    return n;
}
